#include <d2vins/utils.hpp>
#include "d2estimator.hpp" 
#include "unistd.h"
#include "../factors/imu_factor.h"
#include "../factors/depth_factor.h"
#include "../factors/projectionTwoFrameOneCamFactor.h"
#include "../factors/projectionTwoFrameOneCamFactorNoTD.h"
#include <d2frontend/utils.h>

namespace D2VINS {
void D2Estimator::init(ros::NodeHandle & nh) {
    state.init(params->camera_extrinsics, params->td_initial);
    ProjectionTwoFrameOneCamFactor::sqrt_info = params->focal_length / 1.5 * Matrix2d::Identity();
    visual.init(nh, this);
    Swarm::Pose ext = state.getExtrinsic(0);
    printf("[D2VINS::D2Estimator] extrinsic %s\n", ext.toStr().c_str());
}

void D2Estimator::inputImu(IMUData data) {
    imubuf.add(data);
    if (!initFirstPoseFlag) {
        return;
    }
    //Propagation current with last Bias.
}

bool D2Estimator::tryinitFirstPose(const VisualImageDescArray & frame) {
    if (imubuf.size() < params->init_imu_num) {
        return false;
    }
    auto q0 = Utility::g2R(imubuf.mean_acc());
    last_odom = Swarm::Odometry(frame.stamp, Swarm::Pose(q0, Vector3d::Zero()));

    //Easily use the average value as gyrobias now
    //Also the ba with average acc - g
    VINSFrame first_frame(frame, imubuf.mean_acc() - Gravity, imubuf.mean_gyro());
    first_frame.odom = last_odom;

    state.addFrame(frame, first_frame, true);
    
    printf("[D2VINS::D2Estimator] Init pose with IMU: %s\n", last_odom.toStr().c_str());
    printf("[D2VINS::D2Estimator] Gyro bias: %.3f %.3f %.3f\n", first_frame.Bg.x(), first_frame.Bg.y(), first_frame.Bg.z());
    printf("[D2VINS::D2Estimator] Acc  bias: %.3f %.3f %.3f\n\n", first_frame.Ba.x(), first_frame.Ba.y(), first_frame.Ba.z());
    return true;
}

std::pair<bool, Swarm::Pose> D2Estimator::initialFramePnP(const VisualImageDescArray & frame, const Swarm::Pose & pose) {
    //Only use first image for initialization.
    auto & image = frame.images[0];
    std::vector<cv::Point3f> pts3d;
    std::vector<cv::Point2f> pts2d;
    for (auto & lm: image.landmarks) {
        auto & lm_id = lm.landmark_id;
        if (state.hasLandmark(lm_id)) {
            auto & est_lm = state.getLandmarkbyId(lm_id);
            if (est_lm.flag >= LandmarkFlag::INITIALIZED) {
                pts3d.push_back(cv::Point3f(est_lm.position.x(), est_lm.position.y(), est_lm.position.z()));
                pts2d.push_back(cv::Point2f(lm.pt2d_norm.x(), lm.pt2d_norm.y()));
            }
        }
    }

    if (pts3d.size() < params->pnp_min_inliers) {
        return std::make_pair(false, Swarm::Pose());
    }

    cv::Mat inliers;
    cv::Mat D, rvec, t;
    cv::Mat K = (cv::Mat_<double>(3, 3) << 1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0);
    D2FrontEnd::PnPInitialFromCamPose(pose*state.getExtrinsic(0), rvec, t);
    // bool success = cv::solvePnP(pts3d, pts2d, K, D, rvec, t, true);
    bool success = cv::solvePnPRansac(pts3d, pts2d, K, D, rvec, t, true, params->pnp_iteratives,  3, 0.99,  inliers);
    auto pose_cam = D2FrontEnd::PnPRestoCamPose(rvec, t);
    auto pose_imu = pose_cam*state.getExtrinsic(0).inverse();
    // printf("[D2VINS::D2Estimator] PnP initial %s final %s points %d\n", pose.toStr().c_str(), pose_imu.toStr().c_str(), pts3d.size());
    return std::make_pair(success, pose_imu);
}

void D2Estimator::addFrame(const VisualImageDescArray & _frame) {
    //First we init corresponding pose for with IMU
    state.clearFrame();
    if (state.size() > 0) {
        imubuf.pop(state.firstFrame().stamp + state.td);
    }
    auto _imu = imubuf.periodIMU(state.lastFrame().stamp + state.td, _frame.stamp + state.td);
    assert(_imu.size() > 0 && "IMU buffer is empty");
    if (fabs(_imu[_imu.size()-1].t - _frame.stamp - state.td) > params->td_max_diff && frame_count > 10) {
        printf("[D2VINS::D2Estimator] Too large time difference %.3f\n", _imu[_imu.size()-1].t - _frame.stamp - state.td);
        printf("[D2VINS::D2Estimator] Prev frame  %.3f cur   %.3f td %.1fms\n", state.lastFrame().stamp + state.td, _frame.stamp + state.td, state.td*1000);
        printf("[D2VINS::D2Estimator] Imu t_start %.3f t_end %.3f num %d t_last %.3f\n", _imu[0].t, _imu[_imu.size()-1].t, _imu.size(), imubuf[imubuf.size()-1].t);
    }
    VINSFrame frame(_frame, _imu, state.lastFrame());
    if (params->init_method == D2VINSConfig::INIT_POSE_IMU) {
        frame.odom = _imu.propagation(state.lastFrame());
    } else {
        auto odom_imu = _imu.propagation(state.lastFrame());
        auto pnp_init = initialFramePnP(_frame, state.lastFrame().odom.pose());
        if (!pnp_init.first) {
            //Use IMU
            printf("[D2VINS::D2Estimator] Initialization failed, use IMU instead.\n");
        } else {
            odom_imu.pose() = pnp_init.second;
        }
        frame.odom = odom_imu;
    }

    bool is_keyframe = _frame.is_keyframe; //Is keyframe is done in frontend
    state.addFrame(_frame, frame, is_keyframe);

    if (params->verbose) {
        printf("[D2VINS::D2Estimator] Initialize VINSFrame with %d: %s\n", 
            params->init_method, frame.toStr().c_str());
    }
}

void D2Estimator::inputImage(VisualImageDescArray & _frame) {
    if(!initFirstPoseFlag) {
        printf("[D2VINS::D2Estimator] tryinitFirstPose imu buf %ld\n", imubuf.size());
        initFirstPoseFlag = tryinitFirstPose(_frame);
        return;
    }

    double t_imu_frame = _frame.stamp + state.td;
    while (!imubuf.available(t_imu_frame)) {
        //Wait for IMU
        usleep(2000);
        printf("[D2VINS::D2Estimator] wait for imu...\n");
    }

    addFrame(_frame);
    if (state.size() > params->min_solve_frames) {
        solve();
    } else {
        //Presolve only for initialization.
        state.preSolve();
    }
    frame_count ++;
}

void D2Estimator::setStateProperties(ceres::Problem & problem) {
    ceres::EigenQuaternionManifold quat_manifold;
    ceres::EuclideanManifold<3> euc_manifold;
    auto pose_manifold = new ceres::ProductManifold<ceres::EuclideanManifold<3>, ceres::EigenQuaternionManifold>(euc_manifold, quat_manifold);
   
    //set LocalParameterization
    for (size_t i = 0; i < state.size(); i ++ ) {
        auto & frame_a = state.getFrame(i);
        problem.SetManifold(state.getPoseState(frame_a.frame_id), pose_manifold);
    }

    for (int i = 0; i < params->camera_num; i ++) {
        if (!params->estimate_extrinsic) {
            problem.SetParameterBlockConstant(state.getExtrinsicState(i));
        } else {
            problem.SetManifold(state.getExtrinsicState(i), pose_manifold);
        }
    }

    //Current no margarin, fix the first pose
    problem.SetParameterBlockConstant(state.getPoseState(state.firstFrame().frame_id));
    problem.SetParameterBlockConstant(state.getSpdBiasState(state.firstFrame().frame_id));
}

void D2Estimator::solve() {
    solve_count ++;
    state.preSolve();
    ceres::Problem problem;
    setupImuFactors(problem);
    setupLandmarkFactors(problem);
    setStateProperties(problem);

    ceres::Solver::Summary summary;
    ceres::Solve(params->options, &problem, &summary);
    // std::cout << summary.FullReport() << std::endl;
    std::cout << summary.BriefReport() << std::endl;
    state.syncFromState();
    last_odom = state.lastFrame().odom;

    printf("[D2VINS] solve_count %d landmarks %d odom %s td %.1fms opti_time %.1fms\n", solve_count, 
        current_landmark_num, last_odom.toStr().c_str(), state.td*1000, summary.total_time_in_seconds*1000);

    //Reprogation
    auto _imu = imubuf.back(state.lastFrame().stamp + state.td);
    last_prop_odom = _imu.propagation(state.lastFrame());
    visual.postSolve();

    if (params->debug_print_states) {
        state.printSldWin();
    }
}

void D2Estimator::setupImuFactors(ceres::Problem & problem) {
    for (size_t i = 0; i < state.size() - 1; i ++ ) {
        auto & frame_a = state.getFrame(i);
        auto & frame_b = state.getFrame(i + 1);
        auto pre_integrations = frame_b.pre_integrations; //Prev to cuurent
        IMUFactor* imu_factor = new IMUFactor(pre_integrations);
        problem.AddResidualBlock(imu_factor, nullptr, 
            state.getPoseState(frame_a.frame_id), state.getSpdBiasState(frame_a.frame_id), 
            state.getPoseState(frame_b.frame_id), state.getSpdBiasState(frame_b.frame_id));
        //Check the factor
        // printf("[D2VINS::D2Estimator] Add IMU factor %d %d\n", frame_a.frame_id, frame_b.frame_id);
        // printf("Preintegration: relative p %3.2f %3.2f %3.2f\n", pre_integrations[0].delta_p.x(), pre_integrations[0].delta_p.y(), pre_integrations[0].delta_p.z());
        // printf("Preintegration: delta    q %3.2f %3.2f %3.2f %3.2f\n", pre_integrations[0].delta_q.w(), pre_integrations[0].delta_q.x(), pre_integrations[0].delta_q.y(), pre_integrations[0].delta_q.z());
        // auto rp = frame_a.odom.pose().inverse()*frame_b.odom.pose();
        // printf("current relative pose: %s\n", rp.toStr().c_str());
        // std::vector<double*> params{state.getPoseState(frame_a.frame_id), state.getSpdBiasState(frame_a.frame_id), 
        //     state.getPoseState(frame_b.frame_id), state.getSpdBiasState(frame_b.frame_id)};
        // Matrix<double, 15, 1> residuals;
        // imu_factor->testEvaluate(params, residuals.data(), nullptr);
    }
}

void D2Estimator::setupLandmarkFactors(ceres::Problem & problem) {
    auto lms = state.availableLandmarkMeasurements();
    current_landmark_num = lms.size();
    auto loss_function = new ceres::HuberLoss(1.0);    
    for (auto & lm : lms) {
        auto lm_id = lm.landmark_id;
        auto & firstObs = lm.track[0];
        auto mea0 = firstObs.measurement();
        if (firstObs.depth_mea && params->fuse_dep && firstObs.depth < params->max_depth_to_fuse) {
            auto f_dep = OneFrameDepth::Create(firstObs.depth);
            problem.AddResidualBlock(f_dep, loss_function, state.getLandmarkState(lm_id));
        }
        for (auto i = 1; i < lm.track.size(); i++) {
            auto mea1 = lm.track[i].measurement();
            if (params->estimate_td) {
                auto f_td = new ProjectionTwoFrameOneCamFactor(mea0, mea1, firstObs.velocity, lm.track[i].velocity,
                    firstObs.cur_td, lm.track[i].cur_td);
                problem.AddResidualBlock(f_td, loss_function,
                    state.getPoseState(firstObs.frame_id), 
                    state.getPoseState(lm.track[i].frame_id), 
                    state.getExtrinsicState(firstObs.camera_id),
                    state.getLandmarkState(lm_id), &state.td);
            } else {
                ceres::CostFunction * f_lm = nullptr;
                if (lm.track[i].depth_mea && params->fuse_dep && lm.track[i].depth < params->max_depth_to_fuse) {
                    f_lm = ProjectionTwoFrameOneCamFactorNoTD::Create(mea0, mea1, lm.track[i].depth);
                } else {
                    f_lm = ProjectionTwoFrameOneCamFactorNoTD::Create(mea0, mea1);
                }
                problem.AddResidualBlock(f_lm, loss_function,
                    state.getPoseState(firstObs.frame_id), 
                    state.getPoseState(lm.track[i].frame_id), 
                    state.getExtrinsicState(firstObs.camera_id),
                    state.getLandmarkState(lm_id));
                // printf("[D2VINS::D2Estimator] Check landmark %d dep_init/mea %.2f %.2f frame %ld<->%ld\n", 
                //     lm_id, 1/(*state.getLandmarkState(lm_id)), firstObs.depth, lm.track[0].frame_id, lm.track[i].frame_id);
                // ProjectionTwoFrameOneCamFactorNoTD f_test(mea0, mea1, lm.track[i].depth);
                // f_test.test(state.getPoseState(firstObs.frame_id), state.getPoseState(lm.track[i].frame_id), 
                //     state.getExtrinsicState(firstObs.camera_id), state.getLandmarkState(lm_id));
            }
        }
        problem.SetParameterLowerBound(state.getLandmarkState(lm_id), 0, params->min_inv_dep);
    }
    // exit(0);
}

Swarm::Odometry D2Estimator::getImuPropagation() const {
    return last_prop_odom;
}

Swarm::Odometry D2Estimator::getOdometry() const {
    return last_odom;
}

D2EstimatorState & D2Estimator::getState() {
    return state;
}

}