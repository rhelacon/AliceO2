// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file TPCFastTransformHelperO2.cxx
/// \author Sergey Gorbunov

#include "TPCReconstruction/TPCFastTransformHelperO2.h"

#include "TPCBase/Mapper.h"
#include "TPCBase/PadRegionInfo.h"
#include "TPCBase/ParameterDetector.h"
#include "TPCBase/ParameterElectronics.h"
#include "TPCBase/ParameterGas.h"
#include "TPCBase/Sector.h"
#include "DataFormatsTPC/Defs.h"
#include "TPCFastTransform.h"
#include "IrregularSpline2D3DCalibrator.h"
#include "Riostream.h"
#include "FairLogger.h"

using namespace o2::gpu;

namespace o2
{
namespace tpc
{

TPCFastTransformHelperO2* TPCFastTransformHelperO2::sInstance = nullptr;

TPCFastTransformHelperO2* TPCFastTransformHelperO2::instance()
{
  // returns TPCFastTransformHelperO2 instance (singleton)
  if (!sInstance) {
    sInstance = new TPCFastTransformHelperO2();
    sInstance->init();
  }
  return sInstance;
}

void TPCFastTransformHelperO2::init()
{
  // initialize geometry

  // TODO: create TPCFastTransformGeo class

  const Mapper& mapper = Mapper::instance();

  const int nRows = mapper.getNumberOfRows();

  mGeoTransform.startConstruction(nRows);

  TPCDistortionIRS& distortion = mGeoTransform.getDistortionNonConst();

  distortion.startConstruction(nRows, 1);

  auto& detParam = ParameterDetector::Instance();
  float tpcZlengthSideA = detParam.TPClength;
  float tpcZlengthSideC = detParam.TPClength;

  mGeoTransform.setTPCgeometry(tpcZlengthSideA, tpcZlengthSideC);
  distortion.setTPCgeometry(tpcZlengthSideA, tpcZlengthSideC);

  for (int iRow = 0; iRow < mGeoTransform.getNumberOfRows(); iRow++) {
    Sector sector = 0;
    int regionNumber = 0;
    while (iRow >= mapper.getGlobalRowOffsetRegion(regionNumber) + mapper.getNumberOfRowsRegion(regionNumber))
      regionNumber++;

    const PadRegionInfo& region = mapper.getPadRegionInfo(regionNumber);

    int nPads = mapper.getNumberOfPadsInRowSector(iRow);
    float padWidth = region.getPadWidth();

    const GlobalPadNumber pad = mapper.globalPadNumber(PadPos(iRow, nPads / 2));
    const PadCentre& padCentre = mapper.padCentre(pad);
    float xRow = padCentre.X();

    mGeoTransform.setTPCrow(iRow, xRow, nPads, padWidth);
    int scenario = 0;
    distortion.setTPCrow(iRow, xRow, nPads, padWidth, scenario);
  }

  // set calibration parameters to 0., the object is used only for geometrical transformations
  const float t0 = 0.f;
  const float vDrift = 0.f;
  const float vdCorrY = 0.f;
  const float ldCorr = 0.f;
  const float tpcAlignmentZ = 0.f;
  const float tofCorr = 0.f;
  const float primVtxZ = 0.f;
  const long int initTimeStamp = -1;
  mGeoTransform.setCalibration(initTimeStamp, t0, vDrift, vdCorrY, ldCorr, tofCorr, primVtxZ, tpcAlignmentZ);

  IrregularSpline2D3D spline;
  spline.constructRegular(5, 5);
  distortion.setApproximationScenario(0, spline);

  distortion.finishConstruction();
  mGeoTransform.finishConstruction();

  // check if calculated pad geometry is consistent with the map
  testGeometry(mGeoTransform);

  mIsInitialized = 1;
}

std::unique_ptr<TPCFastTransform> TPCFastTransformHelperO2::create(Long_t TimeStamp)
{
  /// initializes TPCFastTransform object

  std::unique_ptr<TPCFastTransform> fastTransformPtr(new TPCFastTransform);

  TPCFastTransform& fastTransform = *fastTransformPtr;
  TPCDistortionIRS& distortion = fastTransform.getDistortionNonConst();

  // tell the transformation to apply the space charge distortions

  // FIXME: this is the awful initialization flag
  constexpr int fastTransformFlag = 1;
  fastTransform.setApplyDistortionFlag(fastTransformFlag);

  if (!mIsInitialized) {
    init();
  }

  const int nRows = mGeoTransform.getNumberOfRows();
  const int nDistortionScenarios = nRows / 10 + 1;
  fastTransform.startConstruction(nRows);
  distortion.startConstruction(nRows, nDistortionScenarios);

  fastTransform.setTPCgeometry(mGeoTransform.getTPCzLengthA(), mGeoTransform.getTPCzLengthC());
  distortion.setTPCgeometry(mGeoTransform.getTPCzLengthA(), mGeoTransform.getTPCzLengthC());

  // init rows
  for (int row = 0; row < mGeoTransform.getNumberOfRows(); row++) {
    const TPCFastTransform::RowInfo& info = mGeoTransform.getRowInfo(row);
    fastTransform.setTPCrow(row, info.x, info.maxPad + 1, info.padWidth);
    int scenario = row / 10;
    if (scenario >= nDistortionScenarios)
      scenario = nDistortionScenarios - 1;
    distortion.setTPCrow(row, info.x, info.maxPad + 1, info.padWidth, scenario);
  }

  // set some initial values, will be reinitialised later int updateCalibration()
  const float t0 = 0.;
  const float vDrift = 0.f;
  const float vdCorrY = 0.;
  const float ldCorr = 0.;
  const float tpcAlignmentZ = 0.;
  const float tofCorr = 0.;
  const float primVtxZ = 0.;
  const long int initTimeStamp = -1;
  fastTransform.setCalibration(initTimeStamp, t0, vDrift, vdCorrY, ldCorr, tofCorr, primVtxZ, tpcAlignmentZ);

  // adjust the number of knots and the knot positions for the TPC distortion splines

  IrregularSpline2D3DCalibrator calibrator;
  calibrator.setRasterSize(41, 41);
  calibrator.setMaxNKnots(21, 21);
  calibrator.setMaximalDeviation(0.01);

  IrregularSpline2D3D raster;
  raster.constructRegular(101, 101);
  std::vector<float> rasterData(3 * raster.getNumberOfKnots());

  for (int scenario = 0; scenario < nDistortionScenarios; scenario++) {
    int row = scenario * 10;
    if (row >= nRows)
      break;
    IrregularSpline2D3D spline;

    if (!mSpaceChargeCorrection) {
      spline.constructRegular(21, 21);
    } else {
      // create the input function
      for (int knot = 0; knot < raster.getNumberOfKnots(); knot++) {
        float su = 0.f, sv = 0.f;
        raster.getKnotUV(knot, su, sv);
        float dx = 0.f, du = 0.f, dv = 0.f;
        const int slice = 0;
        getSpaceChargeCorrection(slice, row, su, sv, dx, du, dv);
        rasterData[3 * knot + 0] = dx;
        rasterData[3 * knot + 1] = du;
        rasterData[3 * knot + 2] = dv;
      }
      raster.correctEdges(rasterData.data());

      std::function<void(float, float, float&, float&, float&)> f;
      f = [&raster, &rasterData](float su, float sv, float& dx, float& du, float& dv) {
        raster.getSpline(rasterData.data(), su, sv, dx, du, dv);
      };

      calibrator.calibrateSpline(spline, f);
      std::cout << "calibrated spline for scenario " << scenario << ", TPC row " << row << ": knots u "
                << spline.getGridU().getNumberOfKnots() << ", v "
                << spline.getGridV().getNumberOfKnots() << std::endl;
    }
    distortion.setApproximationScenario(scenario, spline);
  }
  distortion.finishConstruction();
  fastTransform.finishConstruction();

  // check if calculated pad geometry is consistent with the map
  testGeometry(fastTransform);

  updateCalibration(fastTransform, TimeStamp);

  return std::move(fastTransformPtr);
}

int TPCFastTransformHelperO2::updateCalibration(TPCFastTransform& fastTransform, Long_t TimeStamp)
{
  // Update the calibration with the new time stamp

  if (!mIsInitialized) {
    init();
  }

  Long_t lastTS = fastTransform.getTimeStamp();

  // deinitialize

  fastTransform.setTimeStamp(-1);

  if (TimeStamp < 0) {
    return 0;
  }

  // search for the calibration database ...

  auto& detParam = ParameterDetector::Instance();
  auto& gasParam = ParameterGas::Instance();
  auto& elParam = ParameterElectronics::Instance();

  // calibration found, set the initialized status back

  fastTransform.setTimeStamp(lastTS);

  // less than 60 seconds from the previois time stamp, don't do anything

  if (lastTS >= 0 && TMath::Abs(lastTS - TimeStamp) < 60) {
    return 0;
  }

  // start the initialization

  fastTransform.setTimeStamp(TimeStamp);

  // find last calibrated time bin

  const double vDrift = elParam.ZbinWidth * gasParam.DriftV; // cm/timebin

  //mLastTimeBin = detParam.getTPClength() / vDrift  + 1;

  // fast transform formula:
  // L = (t-t0)*(mVdrift + mVdriftCorrY*yLab ) + mLdriftCorr
  // Z = Z(L) +  tpcAlignmentZ
  // spline distortions for xyz
  // Time-of-flight correction: ldrift += dist-to-vtx*tofCorr

  const double t0 = elParam.PeakingTime / elParam.ZbinWidth;

  const double vdCorrY = 0.;
  const double ldCorr = 0.;
  const double tpcAlignmentZ = 0.;

  const double tofCorr = 0.;
  const double primVtxZ = 0.;

  fastTransform.setCalibration(TimeStamp, t0, vDrift, vdCorrY, ldCorr, tofCorr, primVtxZ, tpcAlignmentZ);

  // now calculate distortion map: dx,du,dv = ( origTransform() -> x,u,v) - fastTransformNominal:x,u,v

  TPCDistortionIRS& distortion = fastTransform.getDistortionNonConst();

  // for the future: switch TOF correction off for a while

  for (int slice = 0; slice < distortion.getNumberOfSlices(); slice++) {
    for (int row = 0; row < distortion.getNumberOfRows(); row++) {
      const IrregularSpline2D3D& spline = distortion.getSpline(slice, row);
      float* data = distortion.getSplineDataNonConst(slice, row);

      for (int knot = 0; knot < spline.getNumberOfKnots(); knot++) {
        float dx = 0.f, du = 0.f, dv = 0.f;
        if (mSpaceChargeCorrection) {
          float su = 0, sv = 0;
          spline.getKnotUV(knot, su, sv);
          getSpaceChargeCorrection(slice, row, su, sv, dx, du, dv);
        }
        data[3 * knot + 0] = dx;
        data[3 * knot + 1] = du;
        data[3 * knot + 2] = dv;
      } // knots
      spline.correctEdges(data);
    } // row
  }   // slice

  // for the future: set back the time-of-flight correction

  return 0;
}

int TPCFastTransformHelperO2::getSpaceChargeCorrection(int slice, int row, float su, float sv, float& dx, float& du, float& dv)
{
  // get space charge correction in internal TPCFastTransform coordinates su,sv->dx,du,dv

  if (!mIsInitialized) {
    init();
  }

  dx = 0.f;
  du = 0.f;
  dv = 0.f;

  if (!mSpaceChargeCorrection) {
    return 0;
  }

  const TPCDistortionIRS& distortion = mGeoTransform.getDistortion();

  const TPCFastTransform::RowInfo& rowInfo = mGeoTransform.getRowInfo(row);

  float x = rowInfo.x;

  // x, u, v cordinates of the (su,sv) point (local cartesian coord. of slice towards central electrode )
  float u = 0, v = 0;
  distortion.convSUVtoUV(slice, row, su, sv, u, v);

  // nominal x,y,z coordinates of the knot (without distortions and time-of-flight correction)
  float y = 0, z = 0;
  mGeoTransform.convUVtoYZ(slice, row, x, u, v, y, z);

  // global coordinates of the knot
  float gx, gy, gz;
  mGeoTransform.convLocalToGlobal(slice, x, y, z, gx, gy, gz);
  float gx1 = gx, gy1 = gy, gz1 = gz;
  {
    double xyz[3] = { gx, gy, gz };
    double dxyz[3] = { 0., 0., 0. };
    mSpaceChargeCorrection(xyz, dxyz);
    gx1 += dxyz[0];
    gy1 += dxyz[1];
    gz1 += dxyz[2];
  }

  // corrections in the local coordinates
  float x1, y1, z1;
  mGeoTransform.convGlobalToLocal(slice, gx1, gy1, gz1, x1, y1, z1);

  // distortion corrections in u,v
  float u1 = 0, v1 = 0;
  mGeoTransform.convYZtoUV(slice, row, x1, y1, z1, u1, v1);

  dx = x1 - x;
  du = u1 - u;
  dv = v1 - v;
  return 0;
}

void TPCFastTransformHelperO2::testGeometry(const TPCFastTransform& fastTransform) const
{
  const Mapper& mapper = Mapper::instance();

  if (fastTransform.getNumberOfSlices() != Sector::MAXSECTOR) {
    LOG(FATAL) << "Wrong number of sectors :" << fastTransform.getNumberOfSlices() << " instead of " << Sector::MAXSECTOR << std::endl;
  }

  if (fastTransform.getNumberOfRows() != mapper.getNumberOfRows()) {
    LOG(FATAL) << "Wrong number of rows :" << fastTransform.getNumberOfRows() << " instead of " << mapper.getNumberOfRows() << std::endl;
  }

  double maxDx = 0, maxDy = 0;

  for (int row = 0; row < fastTransform.getNumberOfRows(); row++) {

    const int nPads = fastTransform.getRowInfo(row).maxPad + 1;

    if (nPads != mapper.getNumberOfPadsInRowSector(row)) {
      LOG(FATAL) << "Wrong number of pads :" << nPads << " instead of " << mapper.getNumberOfPadsInRowSector(row) << std::endl;
    }

    const double x = fastTransform.getRowInfo(row).x;

    // check if calculated pad positions are equal to the real ones

    for (int pad = 0; pad < nPads; pad++) {
      const GlobalPadNumber p = mapper.globalPadNumber(PadPos(row, pad));
      const PadCentre& c = mapper.padCentre(p);
      float u = 0, v = 0;
      int err = fastTransform.convPadTimeToUV(0, row, pad, 10., u, v, 0.);
      if (err != 0) {
        LOG(FATAL) << "Can not transform a cluster: row " << row << " pad " << pad << " time 10. : error " << err << std::endl;
      }

      const double dx = x - c.X();
      const double dy = u - (-c.Y()); // diferent sign convention for Y coordinate in the map

      if (fabs(dx) >= 1.e-6 || fabs(dy) >= 1.e-5) {
        LOG(WARNING) << "wrong calculated pad position:"
                     << " row " << row << " pad " << pad << " x calc " << x << " x in map " << c.X() << " dx " << (x - c.X())
                     << " y calc " << u << " y in map " << -c.Y() << " dy " << dy << std::endl;
      }
      if (fabs(maxDx) < fabs(dx)) {
        maxDx = dx;
      }
      if (fabs(maxDy) < fabs(dy)) {
        maxDy = dy;
      }
    }
  }

  if (fabs(maxDx) >= 1.e-4 || fabs(maxDy) >= 1.e-4) {
    LOG(FATAL) << "wrong calculated pad position:"
               << " max Dx " << maxDx << " max Dy " << maxDy << std::endl;
  }
}
} // namespace tpc
} // namespace o2
