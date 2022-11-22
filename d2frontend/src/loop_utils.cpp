#include <d2frontend/utils.h>
#include <opencv2/opencv.hpp>
#include <opencv2/core/eigen.hpp>
#include <fstream>
#include <d2common/d2basetypes.h>
#include <d2common/utils.hpp>
#include <d2frontend/d2frontend_params.h>
#include <opengv/sac_problems/absolute_pose/AbsolutePoseSacProblem.hpp>
#include <opengv/absolute_pose/methods.hpp>
#include <opengv/absolute_pose/NoncentralAbsoluteAdapter.hpp>
#include <opengv/sac/Ransac.hpp>
#include <opengv/sac/Lmeds.hpp>
#include <d2frontend/loop_detector.h>
#include <opencv2/cudaoptflow.hpp>
#include <opencv2/cudaimgproc.hpp>

using namespace std::chrono; 
using namespace D2Common;
using D2Common::Utility::TicToc;

#define PYR_LEVEL 3
#define WIN_SIZE cv::Size(21, 21)

namespace D2FrontEnd {
cv::Mat getImageFromMsg(const sensor_msgs::CompressedImageConstPtr &img_msg, int flag) {
    return cv::imdecode(img_msg->data, flag);
}

cv_bridge::CvImagePtr getImageFromMsg(const sensor_msgs::Image &img_msg)
{
    cv_bridge::CvImagePtr ptr;
    // std::cout << img_msg->encoding << std::endl;
    if (img_msg.encoding == "8UC1" || img_msg.encoding == "mono8")
    {
        ptr = cv_bridge::toCvCopy(img_msg, "8UC1");
    } else if (img_msg.encoding == "16UC1" || img_msg.encoding == "mono16") {
        ptr = cv_bridge::toCvCopy(img_msg, "16UC1");
        ptr->image.convertTo(ptr->image, CV_8UC1, 1.0/256.0);
    } else {
        ptr = cv_bridge::toCvCopy(img_msg, sensor_msgs::image_encodings::BGR8);        
    }
    return ptr;
}

cv_bridge::CvImagePtr getImageFromMsg(const sensor_msgs::ImageConstPtr &img_msg)
{
    cv_bridge::CvImagePtr ptr;
    // std::cout << img_msg->encoding << std::endl;
    if (img_msg->encoding == "8UC1" || img_msg->encoding == "mono8")
    {
        ptr = cv_bridge::toCvCopy(img_msg, "8UC1");
    } else if (img_msg->encoding == "16UC1" || img_msg->encoding == "mono16") {
        ptr = cv_bridge::toCvCopy(img_msg, "16UC1");
        ptr->image.convertTo(ptr->image, CV_8UC1, 1.0/256.0);
    } else {
        ptr = cv_bridge::toCvCopy(img_msg, sensor_msgs::image_encodings::BGR8);        
    }
    return ptr;
}

Swarm::Pose AffineRestoCamPose(Eigen::Matrix4d affine) {
    Eigen::Matrix3d R;
    Eigen::Vector3d T;

    R = affine.block<3, 3>(0, 0);
    T = affine.block<3, 1>(0, 3);
    
    R = (R.normalized()).transpose();
    T = R *(-T);

    std::cout << "R of affine\n" << R << std::endl;
    std::cout << "T of affine\n" << T << std::endl;
    std::cout << "RtR\n" << R.transpose()*R << std::endl;
    return Swarm::Pose(R, T);
}

void PnPInitialFromCamPose(const Swarm::Pose &p, cv::Mat & rvec, cv::Mat & tvec) {
    Eigen::Matrix3d R_w_c = p.R();
    Eigen::Matrix3d R_inital = R_w_c.inverse();
    Eigen::Vector3d T_w_c = p.pos();
    cv::Mat tmp_r;
    Eigen::Vector3d P_inital = -(R_inital * T_w_c);

    cv::eigen2cv(R_inital, tmp_r);
    cv::Rodrigues(tmp_r, rvec);
    cv::eigen2cv(P_inital, tvec);
}

Swarm::Pose PnPRestoCamPose(cv::Mat rvec, cv::Mat tvec) {
    cv::Mat r;
    cv::Rodrigues(rvec, r);
    Eigen::Matrix3d R_pnp, R_w_c_old;
    cv::cv2eigen(r, R_pnp);
    R_w_c_old = R_pnp.transpose();
    Eigen::Vector3d T_pnp, T_w_c_old;
    cv::cv2eigen(tvec, T_pnp);
    T_w_c_old = R_w_c_old * (-T_pnp);

    return Swarm::Pose(R_w_c_old, T_w_c_old);
}

cv::Vec3b extractColor(const cv::Mat &img, cv::Point2f p) {
    cv::Vec3b color;
    if (img.channels() == 3) {
        color = img.at<cv::Vec3b>(p);
    } else {
        auto grayscale = img.at<uchar>(p);
        color = cv::Vec3b(grayscale, grayscale, grayscale);
    }
    return color;
}


#define MAXBUFSIZE 100000
Eigen::MatrixXf load_csv_mat_eigen(std::string csv) {
    int cols = 0, rows = 0;
    double buff[MAXBUFSIZE];

    // Read numbers from file into buffer.
    std::ifstream infile;
    infile.open(csv);
    std::string line;

    while (getline(infile, line))
    {
        int temp_cols = 0;
        std::stringstream          lineStream(line);
        std::string                cell;

        while (std::getline(lineStream, cell, ','))
        {
            buff[rows * cols + temp_cols] = std::stod(cell);
            temp_cols ++;
        }

        rows ++;
        if (cols > 0) {
            assert(cols == temp_cols && "Matrix must have same cols on each rows!");
        } else {
            cols = temp_cols;
        }
    }

    infile.close();

    Eigen::MatrixXf result(rows,cols);
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++)
            result(i,j) = buff[ cols*i+j ];

    return result;
}

Eigen::VectorXf load_csv_vec_eigen(std::string csv) {
    int cols = 0, rows = 0;
    double buff[MAXBUFSIZE];

    // Read numbers from file into buffer.
    std::ifstream infile;
    infile.open(csv);
    while (! infile.eof())
    {
        std::string line;
        getline(infile, line);

        int temp_cols = 0;
        std::stringstream stream(line);
        while(! stream.eof())
            stream >> buff[cols*rows+temp_cols++];

        if (temp_cols == 0)
            continue;

        if (cols == 0)
            cols = temp_cols;

        rows++;
    }

    infile.close();

    rows--;

    // Populate matrix with numbers.
    Eigen::VectorXf result(rows,cols);
    for (int i = 0; i < rows; i++)
            result(i) = buff[ i ];

    return result;
}

cv::Point2f rotate_pt_norm2d(cv::Point2f pt, Eigen::Quaterniond q) {
    Eigen::Vector3d pt3d(pt.x, pt.y, 1);
    pt3d = q * pt3d;

    if (pt3d.z() < 1e-3 && pt3d.z() > 0) {
        pt3d.z() = 1e-3;
    }

    if (pt3d.z() > -1e-3 && pt3d.z() < 0) {
        pt3d.z() = -1e-3;
    }

    return cv::Point2f(pt3d.x()/ pt3d.z(), pt3d.y()/pt3d.z());
}


bool inBorder(const cv::Point2f &pt, cv::Size shape)
{
    const int BORDER_SIZE = 1;
    int img_x = cvRound(pt.x);
    int img_y = cvRound(pt.y);
    return BORDER_SIZE <= img_x && img_x < shape.width - BORDER_SIZE && BORDER_SIZE <= img_y && img_y < shape.height - BORDER_SIZE;
}

std::vector<cv::DMatch> matchKNN(const cv::Mat & desc_a, const cv::Mat & desc_b, double knn_match_ratio, 
        const std::vector<cv::Point2f> pts_a,
        const std::vector<cv::Point2f> pts_b,
        double search_local_dist) {
    //Match descriptors with OpenCV knnMatch
    std::vector<std::vector<cv::DMatch>> matches;
    cv::BFMatcher bfmatcher(cv::NORM_L2);
    bfmatcher.knnMatch(desc_a, desc_b, matches, 2);
    std::vector<cv::DMatch> good_matches;
    for (auto & match : matches) {
        if (match.size() < 2) {
            continue;
        }
        if (match[0].distance < knn_match_ratio * match[1].distance) {
            if (search_local_dist > 0) {
                if (cv::norm(pts_a[match[0].queryIdx] - pts_b[match[0].trainIdx]) > search_local_dist) {
                    continue;
                }
            }
            good_matches.push_back(match[0]);
        }
    }
    return good_matches;
}


std::vector<cv::Point2f> opticalflowTrack(const cv::Mat & cur_img, const cv::Mat & prev_img, std::vector<cv::Point2f> & prev_pts, 
        std::vector<LandmarkIdType> & ids, TrackLRType type, bool enable_cuda) {
    if (prev_pts.size() == 0) {
        return std::vector<cv::Point2f>();
    }
    TicToc tic;
    std::vector<uchar> status;
    std::vector<cv::Point2f> cur_pts;
    float move_cols = cur_img.cols*90.0/params->undistort_fov; //slightly lower than 0.5 cols when fov=200

    if (prev_pts.size() == 0) {
        return std::vector<cv::Point2f>();
    }

    if (type == WHOLE_IMG_MATCH) {
        cur_pts = prev_pts;
    } else  {
        status.resize(prev_pts.size());
        std::fill(status.begin(), status.end(), 0);
        if (type == LEFT_RIGHT_IMG_MATCH) {
            for (unsigned int i = 0; i < prev_pts.size(); i++) {
                auto pt = prev_pts[i];
                if (pt.x < cur_img.cols - move_cols) {
                    pt.x += move_cols;
                    status[i] = 1;
                    cur_pts.push_back(pt);
                }
            }
        } else {
            for (unsigned int i = 0; i < prev_pts.size(); i++) {
                auto pt = prev_pts[i];
                if (pt.x >= move_cols) {
                    pt.x -= move_cols;
                    status[i] = 1;
                    cur_pts.push_back(pt);
                }
            }
        }
        reduceVector(prev_pts, status);
        reduceVector(ids, status);
    }
    status.resize(0);
    if (cur_pts.size() == 0) {
        return std::vector<cv::Point2f>();
    }
    std::vector<float> err;
    std::vector<uchar> reverse_status;
    std::vector<cv::Point2f> reverse_pts;
    if (enable_cuda) {
        cv::cuda::GpuMat gpu_prev_img(prev_img);
        cv::cuda::GpuMat gpu_cur_img(cur_img);
        cv::cuda::GpuMat gpu_prev_pts(prev_pts);
        cv::cuda::GpuMat gpu_cur_pts(cur_pts);
        cv::cuda::GpuMat gpu_status;
        cv::cuda::GpuMat reverse_gpu_status;
        cv::Ptr<cv::cuda::SparsePyrLKOpticalFlow> d_pyrLK_sparse = cv::cuda::SparsePyrLKOpticalFlow::create(WIN_SIZE, PYR_LEVEL, 30, true);
        d_pyrLK_sparse->calc(gpu_prev_img, gpu_cur_img, gpu_prev_pts, gpu_cur_pts, gpu_status);
        gpu_status.download(status);
        gpu_cur_pts.download(cur_pts);
        reverse_pts = cur_pts;
        for (unsigned int i = 0; i < prev_pts.size(); i++) {
            auto & pt = reverse_pts[i];
            if (type == LEFT_RIGHT_IMG_MATCH && status[i] == 1) {
                pt.x -= move_cols;
            }
            if (type == RIGHT_LEFT_IMG_MATCH && status[i] == 1) {
                pt.x += move_cols;
            }
        }
        cv::cuda::GpuMat reverse_gpu_pts(reverse_pts);
        d_pyrLK_sparse->calc(gpu_cur_img, gpu_prev_img, gpu_cur_pts, reverse_gpu_pts, reverse_gpu_status);
        reverse_gpu_pts.download(reverse_pts);
        reverse_gpu_status.download(reverse_status);
    } else {
        cv::calcOpticalFlowPyrLK(prev_img, cur_img, prev_pts, cur_pts, status, err, WIN_SIZE, PYR_LEVEL, 
                cv::TermCriteria(cv::TermCriteria::COUNT+cv::TermCriteria::EPS, 30, 0.01), cv::OPTFLOW_USE_INITIAL_FLOW);
        reverse_pts = cur_pts;
        for (unsigned int i = 0; i < prev_pts.size(); i++) {
            auto & pt = reverse_pts[i];
            if (type == LEFT_RIGHT_IMG_MATCH && status[i] == 1) {
                pt.x -= move_cols;
            }
            if (type == RIGHT_LEFT_IMG_MATCH && status[i] == 1) {
                pt.x += move_cols;
            }
        }
        cv::calcOpticalFlowPyrLK(cur_img, prev_img, cur_pts, reverse_pts, reverse_status, err, WIN_SIZE, PYR_LEVEL, 
                cv::TermCriteria(cv::TermCriteria::COUNT+cv::TermCriteria::EPS, 30, 0.01), cv::OPTFLOW_USE_INITIAL_FLOW);
    }
    

    for(size_t i = 0; i < status.size(); i++)
    {
        if(status[i] && reverse_status[i] && cv::norm(prev_pts[i] - reverse_pts[i]) <= 0.5)
        {
            status[i] = 1;
        }
        else
            status[i] = 0;
    }

    for (int i = 0; i < int(cur_pts.size()); i++){
        if (status[i] && !inBorder(cur_pts[i], cur_img.size())) {
            status[i] = 0;
        }
    }   
    reduceVector(prev_pts, status);
    reduceVector(cur_pts, status);
    reduceVector(ids, status);
    return cur_pts;
} 


void detectPoints(const cv::Mat & img, std::vector<cv::Point2f> & n_pts, std::vector<cv::Point2f> & cur_pts, int require_pts, bool enable_cuda) {
    int lack_up_top_pts = require_pts - static_cast<int>(cur_pts.size());
    cv::Mat mask;
    if (params->enable_perf_output) {
        ROS_INFO("Lost %d pts; Require %d will detect %d", lack_up_top_pts, require_pts, lack_up_top_pts > require_pts/4);
    }
    if (lack_up_top_pts > require_pts/4) {
        cv::Mat d_prevPts;
        if (enable_cuda) {
            cv::Ptr<cv::cuda::CornersDetector> detector = cv::cuda::createGoodFeaturesToTrackDetector(
                img.type(), lack_up_top_pts, 0.01, params->feature_min_dist);
            cv::cuda::GpuMat d_prevPts_gpu;
            cv::cuda::GpuMat img_cuda(img);
            detector->detect(img_cuda, d_prevPts_gpu);
            d_prevPts_gpu.download(d_prevPts);
        } else {
            cv::goodFeaturesToTrack(img, d_prevPts, lack_up_top_pts, 0.01, params->feature_min_dist, mask);
        }
        std::vector<cv::Point2f> n_pts_tmp;
        // std::cout << "d_prevPts size: "<< d_prevPts.size()<<std::endl;
        if(!d_prevPts.empty()) {
            n_pts_tmp = cv::Mat_<cv::Point2f>(cv::Mat(d_prevPts));
        }
        else {
            n_pts_tmp.clear();
        }
        n_pts.clear();
        std::vector<cv::Point2f> all_pts = cur_pts;
        for (auto & pt : n_pts_tmp) {
            bool has_nearby = false;
            for (auto &pt_j: all_pts) {
                if (cv::norm(pt-pt_j) < params->feature_min_dist) {
                    has_nearby = true;
                    break;
                }
            }
            if (!has_nearby) {
                n_pts.push_back(pt);
                all_pts.push_back(pt);
            }
        }
    }
    else {
        n_pts.clear();
    }
}  

bool pnp_result_verify(bool pnp_success, int inliers, double rperr, const Swarm::Pose & DP_old_to_new) {
    bool success = pnp_success;
    if (!pnp_success) {
        return false;
    }
    if (rperr > params->loopdetectorconfig->gravity_check_thres) {
        printf("[SWARM_LOOP] Check failed on RP error %.1fdeg (%.1f)deg\n", rperr*57.3, params->loopdetectorconfig->gravity_check_thres*57.3);
        return false;
    }   
    auto &_config = (*params->loopdetectorconfig);
    success = (inliers >= _config.loop_inlier_feature_num) && fabs(DP_old_to_new.yaw()) < _config.accept_loop_max_yaw*DEG2RAD && DP_old_to_new.pos().norm() < _config.accept_loop_max_pos;
    return success;
}

double gravityCheck(const Swarm::Pose & pnp_pose, const Swarm::Pose & ego_motion) {
    //This checks the gravity direction
    Vector3d gravity(0, 0, 1);
    Vector3d gravity_pnp = pnp_pose.R().inverse() * gravity;
    Vector3d gravity_ego = ego_motion.R().inverse() * gravity;
    double sin_theta = gravity_pnp.cross(gravity_ego).norm();
    return sin_theta;
}

int computeRelativePosePnP(const std::vector<Vector3d> lm_positions_a, const std::vector<Vector3d> lm_3d_norm_b,
            Swarm::Pose extrinsic_b, Swarm::Pose ego_motion_a, Swarm::Pose ego_motion_b, Swarm::Pose & DP_b_to_a, std::vector<int> &inliers, 
            bool is_4dof, bool verify_gravity) {
        //Compute PNP
    // ROS_INFO("Matched features %ld", matched_2d_norm_old.size());
    cv::Mat K = (cv::Mat_<double>(3, 3) << 1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0);
    cv::Mat r, rvec, rvec2, t, t2, D, tmp_r;

    int iteratives = 100;
    Point3fVector pts3d;
    Point2fVector pts2d;
    for (int i = 0; i < lm_positions_a.size(); i++) {
        auto z = lm_3d_norm_b[i].z();
        if (z > 1e-1) {
            pts3d.push_back(cv::Point3f(lm_positions_a[i].x(), lm_positions_a[i].y(), lm_positions_a[i].z()));
            pts2d.push_back(cv::Point2f(lm_3d_norm_b[i].x()/z, lm_3d_norm_b[i].y()/z));
        }
    }
    if (pts3d.size() < params->loopdetectorconfig->loop_inlier_feature_num) {
        return false;
    }
    bool success = solvePnPRansac(pts3d, pts2d, K, D, rvec, t, false,   
        iteratives,  5.0/params->focal_length, 0.99,  inliers);
    auto p_cam_old_in_new = PnPRestoCamPose(rvec, t);
    auto pnp_predict_pose_b = p_cam_old_in_new*(extrinsic_b.toIsometry().inverse());
    if (!success) {
        return 0;
    }
    DP_b_to_a =  Swarm::Pose::DeltaPose(pnp_predict_pose_b, ego_motion_a, is_4dof);
    if (verify_gravity) {
        auto RPerr = gravityCheck(pnp_predict_pose_b, ego_motion_b);
        success = pnp_result_verify(success, inliers.size(), RPerr, DP_b_to_a);
        printf("[SWARM_LOOP@%d] DPose %s PnPRansac %d inlines %d/%d, dyaw %f dpos %f g_err %f \n",
                params->self_id, DP_b_to_a.toStr().c_str(), success, inliers.size(), pts2d.size(), fabs(DP_b_to_a.yaw())*57.3, DP_b_to_a.pos().norm(), RPerr);
    }
    return success;
}

Swarm::Pose computePosePnPnonCentral(const std::vector<Vector3d> & lm_positions_a, const std::vector<Vector3d> & lm_3d_norm_b,
        const std::vector<Swarm::Pose> & cam_extrinsics, const std::vector<int> & camera_indices, std::vector<int> &inliers) {
    opengv::bearingVectors_t bearings;
    std::vector<int> camCorrespondences;
    opengv::points_t points;
    opengv::rotations_t camRotations;
    opengv::translations_t camOffsets;
    for (int i = 0; i < lm_positions_a.size(); i++) {
        bearings.push_back(lm_3d_norm_b[i]);
        camCorrespondences.push_back(camera_indices[i]);
        points.push_back(lm_positions_a[i]);
    }
    for (int i = 0; i < cam_extrinsics.size(); i++) {
        camRotations.push_back(cam_extrinsics[i].R());
        camOffsets.push_back(cam_extrinsics[i].pos());
    }

    //Solve with GP3P + RANSAC
    opengv::absolute_pose::NoncentralAbsoluteAdapter adapter(
        bearings, camCorrespondences, points, camOffsets, camRotations);
    opengv::sac::Ransac<
        opengv::sac_problems::absolute_pose::AbsolutePoseSacProblem> ransac;
    std::shared_ptr<opengv::sac_problems::absolute_pose::AbsolutePoseSacProblem> absposeproblem_ptr(new opengv::sac_problems::absolute_pose::AbsolutePoseSacProblem(
        adapter, opengv::sac_problems::absolute_pose::AbsolutePoseSacProblem::GP3P));
    ransac.sac_model_ = absposeproblem_ptr;
    // ransac.threshold_ = 1.0 - cos(atan(sqrt(10.0)*0.5/460.0));
    ransac.threshold_ = 1.0/params->focal_length;
    ransac.max_iterations_ = 50;
    ransac.computeModel();
    //Obtain relative pose results
    inliers = ransac.inliers_;
    auto best_transformation = ransac.model_coefficients_;
    Matrix3d R = best_transformation.block<3, 3>(0, 0);
    Vector3d t = best_transformation.block<3, 1>(0, 3);
    Swarm::Pose p_drone_old_in_new_init(R, t);
    
    //Filter by inliers and perform non-linear optimization to refine.
    std::set <int>inlier_set(inliers.begin(), inliers.end());
    bearings.clear();
    camCorrespondences.clear();
    points.clear();
    for (int i = 0; i < lm_positions_a.size(); i++) {
        if (inlier_set.find(i) == inlier_set.end()) {
            continue;
        }
        bearings.push_back(lm_3d_norm_b[i]);
        camCorrespondences.push_back(camera_indices[i]);
        points.push_back(lm_positions_a[i]);
    }
    adapter.sett(t);
    adapter.setR(R);
    opengv::transformation_t nonlinear_transformation =
        opengv::absolute_pose::optimize_nonlinear(adapter);
    R = nonlinear_transformation.block<3, 3>(0, 0);
    t = nonlinear_transformation.block<3, 1>(0, 3);
    Swarm::Pose p_drone_old_in_new(R, t);
    return p_drone_old_in_new;
}

int computeRelativePosePnPnonCentral(const std::vector<Vector3d> & lm_positions_a, const std::vector<Vector3d> & lm_3d_norm_b,
        const std::vector<Swarm::Pose> & cam_extrinsics, const std::vector<int> & camera_indices, 
        Swarm::Pose drone_pose_a, Swarm::Pose ego_motion_b, 
        Swarm::Pose & DP_b_to_a, std::vector<int> &inliers, bool is_4dof, bool verify_gravity) {
    D2Common::Utility::TicToc tic;
    auto pnp_predict_pose_b = computePosePnPnonCentral(lm_positions_a, lm_3d_norm_b, cam_extrinsics, camera_indices, inliers);
    DP_b_to_a =  Swarm::Pose::DeltaPose(pnp_predict_pose_b, drone_pose_a, is_4dof);

    bool success = true;
    double RPerr = 0;
    if (verify_gravity) {
    //Verify the results
        auto RPerr = gravityCheck(pnp_predict_pose_b, ego_motion_b);
        success = pnp_result_verify(true, inliers.size(), RPerr, DP_b_to_a);
    }
    
    if (params->enable_perf_output) {
        printf("[SWARM_LOOP@%d] features %d/%d succ %d gPnPRansac time %.2fms RP: %s g_err %f\n", params->self_id, inliers.size(), lm_3d_norm_b.size(), success,
                tic.toc(), DP_b_to_a.toStr().c_str(), RPerr);
    }
    return success;
}

}