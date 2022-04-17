#include <d2vins/d2vins_types.hpp>
#include <d2vins/d2vins_params.hpp>
#include "MSCKF_state.hpp"

namespace D2VINS {
class MSCKF {
    MSCKFStateVector nominal_state;
    MSCKFErrorStateVector error_state;
    D2VINSConfig _config;
    double t_last = -1;
    bool initFirstPoseFlag = false;

    IMUBuffer imubuf;

    Eigen::Matrix<double, IMU_NOISE_DIM, IMU_NOISE_DIM> Q_imu;

public:
    MSCKF();
    void initFirstPose();
    void predict(const double t, const IMUData & imudata);
    void addKeyframe(const double t); //For convience, we require t here is exact same to last imu t
    void update(const D2FrontEnd::LandmarkPerId & feature_by_id);
};
}