#include <d2frontend/d2frontend.h>
#include <d2frontend/utils.h>
#include <d2frontend/d2featuretracker.h>
#include "ros/ros.h"
#include <iostream>
#include "d2frontend/loop_net.h"
#include "d2frontend/loop_cam.h"
#include "d2frontend/loop_detector.h"
#include <Eigen/Eigen>
#include <thread>
#include <nav_msgs/Odometry.h>
#include <mutex>
#include <swarm_msgs/node_frame.h>
#include <d2common/utils.hpp>
#include <image_transport/image_transport.h>

// #define BACKWARD_HAS_DW 1
// #include <backward.hpp>
// namespace backward
// {
//     backward::SignalHandling sh;
// }

namespace D2FrontEnd {
typedef std::lock_guard<std::mutex> lock_guard;

void D2Frontend::log_fe_time(double duration, std::string fname) {
    std::string path = "/root/output//" + fname;
    //output the duration of the frame.
    std::fstream file;
    file.open(path.c_str(), std::fstream::app);
    file << std::fixed << duration << std::endl;
    file.close();
}

void D2Frontend::onLoopConnection(LoopEdge & loop_con, bool is_local) {
    if(is_local && params->pgo_mode == PGO_MODE::PGO_MODE_NON_DIST) {
        //Only PGO is non-distributed we broadcast the loops.
        loop_net->broadcastLoopConnection(loop_con);
    }

    // ROS_INFO("Pub loop conn. is local %d", is_local);
    loopconn_pub.publish(loop_con);
}

StereoFrame D2Frontend::findImagesRaw(const nav_msgs::Odometry & odometry) {
    // ROS_INFO("findImagesRaw %f", odometry.header.stamp.toSec());
    auto stamp = odometry.header.stamp;
    StereoFrame ret;
    raw_stereo_image_lock.lock();
    while (raw_stereo_images.size() > 0 && stamp.toSec() - raw_stereo_images.front().stamp.toSec() > 1e-3) {
        // ROS_INFO("Removing d stamp %f", stamp.toSec() - raw_stereo_images.front().stamp.toSec());
        raw_stereo_images.pop();
    }

    if (raw_stereo_images.size() > 0 && fabs(stamp.toSec() - raw_stereo_images.front().stamp.toSec()) < 1e-3) {
        auto ret = raw_stereo_images.front();
        raw_stereo_images.pop();
        ret.pose_drone = odometry.pose.pose;
        // ROS_INFO("VIO KF found, returning...");
        raw_stereo_image_lock.unlock();
        return ret;
    } 

    raw_stereo_image_lock.unlock();
    return ret;
}

void D2Frontend::stereoImagesCallback(const sensor_msgs::ImageConstPtr left, const sensor_msgs::ImageConstPtr right) {
    auto _l = getImageFromMsg(left);
    auto _r = getImageFromMsg(right);
    StereoFrame sframe(_l->header.stamp, _l->image, _r->image, params->extrinsics[0], params->extrinsics[1], params->self_id);
    processStereoframe(sframe);
}

void D2Frontend::depthImagesCallback(const sensor_msgs::ImageConstPtr left, const sensor_msgs::ImageConstPtr depth) {
    auto _l = getImageFromMsg(left);
    auto _d = getImageFromMsg(depth);
    StereoFrame sframe(left->header.stamp, _l->image, _d->image, params->extrinsics[0], params->self_id);
    processStereoframe(sframe);
}

void D2Frontend::monoImageCallback(const sensor_msgs::ImageConstPtr & image) {
    auto _l = getImageFromMsg(image);
    auto img = _l->image;
    //Horizon split image to four images:
    std::vector<cv::Mat> imgs;
    const int num_imgs = 4;
    for (int i = 0; i < 4; i++) {
        imgs.emplace_back(img(cv::Rect(i * img.cols /num_imgs, 0, img.cols /num_imgs, img.rows)));
        if (imgs.back().channels() == 3) {
            cv::cvtColor(imgs.back(), imgs.back(), cv::COLOR_BGR2GRAY);
        }
    }
    if (params->show_raw_image) {
        cv::namedWindow("raw_image", cv::WINDOW_NORMAL | cv::WINDOW_GUI_EXPANDED);
        cv::imshow("RawImage", img);
    }
    StereoFrame sframe(image->header.stamp, imgs, params->extrinsics, params->self_id);
    processStereoframe(sframe);
}

void D2Frontend::processStereoframe(const StereoFrame & stereoframe) {
    double t_fe_processStereoframe_start = ros::Time::now().toSec();
    static int count = 0;
    // ROS_INFO("[D2Frontend::processStereoframe] %d", count ++);
    auto vframearry = loop_cam->processStereoframe(stereoframe);
    vframearry.motion_prediction = getMotionPredict(vframearry.stamp);
    bool is_keyframe = feature_tracker->trackLocalFrames(vframearry);
    vframearry.prevent_adding_db = !is_keyframe;
    vframearry.is_keyframe = is_keyframe;
    received_image = true;

    double t_fe_processStereoframe_end = ros::Time::now().toSec();
    double duration_fe_processStereoframe = t_fe_processStereoframe_end - t_fe_processStereoframe_start;
    log_fe_time(t_fe_processStereoframe_start, "t_fe_processStereoframe_start.txt");
    log_fe_time(duration_fe_processStereoframe, "d_fe_processStereoframe.txt");

    if (!params->show) {
        vframearry.releaseRawImages();
    }
    if (vframearry.send_to_backend) {
        backendFrameCallback(vframearry);
    }
}

void D2Frontend::addToLoopQueue(const VisualImageDescArray & viokf) {
    if (params->enable_loop) {
        lock_guard guard(loop_lock);
        Utility::TicToc tic;
        loop_queue.push(viokf);
    }
}

void D2Frontend::onRemoteImage(VisualImageDescArray frame_desc) {
    if (frame_desc.is_lazy_frame || frame_desc.matched_frame >= 0) {
        processRemoteImage(frame_desc, false);
    } else {
        bool succ = feature_tracker->trackRemoteFrames(frame_desc);
        processRemoteImage(frame_desc, succ);
    }
}

void D2Frontend::processRemoteImage(VisualImageDescArray & frame_desc, bool succ_track) {
    if (params->enable_loop) {
        if (!frame_desc.isMatchedFrame()) {
            if (params->verbose)
                printf("[D2Frontend] Remote image %d is not matched, directly pass to detector\n", frame_desc.frame_id);
            //Check if keyframe!!!
            if (frame_desc.is_keyframe) {
                addToLoopQueue(frame_desc);
            }
        } else {
            //We need to wait the matched frame is added to loop detector.
            if (loop_detector->hasFrame(frame_desc.matched_frame) || frame_desc.matched_drone != params->self_id) {
                if (params->verbose) {
                    printf("[D2Frontend] Remote image %d is matched with %d drone %d add to loop queue\n", 
                            frame_desc.frame_id, frame_desc.matched_frame, frame_desc.drone_id);
                }
                addToLoopQueue(frame_desc);
            } else {
                VisualImageDescArray _frame_desc = frame_desc;
                if (params->verbose)
                    printf("[D2Frontend] Remote image %d is matched with %d, waiting for matched frame\n", frame_desc.frame_id, frame_desc.matched_frame);
                new std::thread([&](VisualImageDescArray frame) {
                    int count = 0;
                    while (count < 1000) {
                        if (loop_detector->hasFrame(frame.matched_frame)) {
                            printf("[D2Frontend] frame %ld waited %d us for matched frame %d\n", frame.frame_id, count * 1000, 
                                    frame.matched_frame);
                            addToLoopQueue(frame);
                            break;
                        }
                        usleep(1000);
                        count += 1;
                    }
                }, (_frame_desc));
            }
        }
    }
}


void D2Frontend::loopDetectionThread() {
    while (ros::ok()) {
        if (loop_queue.size() > 0) {
            VisualImageDescArray vframearry;
            {
                lock_guard guard(loop_lock);
                vframearry = loop_queue.front();
                loop_queue.pop();
            }
            if (loop_queue.size() > 10) {
                ROS_WARN("[D2Frontend] Loop queue size is %d", loop_queue.size());
            }
            loop_detector->processImageArray(vframearry);
        }
        usleep(10000);
    }
}

void D2Frontend::pubNodeFrame(const VisualImageDescArray & viokf) {
    auto _kf = viokf.toROS();
    keyframe_pub.publish(_kf);
}

void D2Frontend::onRemoteFrameROS(const swarm_msgs::ImageArrayDescriptor & remote_img_desc) {
    // ROS_INFO("Remote");
    if (received_image) {
        this->onRemoteImage(remote_img_desc);
    }
}

D2Frontend::D2Frontend () {}

void D2Frontend::Init(ros::NodeHandle & nh) {
    //Init Loop Net
    params = new D2FrontendParams(nh);
    it_ = new image_transport::ImageTransport(nh);
    cv::setNumThreads(1);

    loop_net = new LoopNet(params->_lcm_uri, params->send_img, params->send_whole_img_desc, params->recv_msg_duration);
    loop_cam = new LoopCam(*(params->loopcamconfig), nh);
    feature_tracker = new D2FeatureTracker(*(params->ftconfig));
    feature_tracker->cams = loop_cam->cams;
    loop_detector = new LoopDetector(params->self_id, *(params->loopdetectorconfig));
    loop_detector->loop_cam = loop_cam;

    loop_detector->on_loop_cb = [&] (LoopEdge & loop_con) {
        this->onLoopConnection(loop_con, true);
    };

    loop_detector->broadcast_keyframe_cb = [&] (VisualImageDescArray & viokf) {
        loop_net->broadcastVisualImageDescArray(viokf, true);
    };

    loop_net->frame_desc_callback = [&] (const VisualImageDescArray & frame_desc) {
        if (received_image) {
            if (params->enable_pub_remote_frame) {
                remote_image_desc_pub.publish(frame_desc.toROS());
            }
            this->onRemoteImage(frame_desc);
            this->pubNodeFrame(frame_desc);
        }
    };

    loop_net->loopconn_callback = [&] (const LoopEdge_t & loop_conn) {
        auto loc = toROSLoopEdge(loop_conn);
        onLoopConnection(loc, false);
    };
    std::string format = "raw";
    if (params->is_comp_images) {
        format = "compressed";
    }
    image_transport::TransportHints hints(format, ros::TransportHints().tcpNoDelay(true));
    if (params->camera_configuration == CameraConfig::STEREO_PINHOLE || params->camera_configuration == CameraConfig::STEREO_FISHEYE) {
        ROS_INFO("[D2Frontend] Input: images %s and %s", params->image_topics[0].c_str(), params->image_topics[1].c_str());
        image_sub_l = new ImageSubscriber(*it_, params->image_topics[0], 1000, hints);
        image_sub_r = new ImageSubscriber(*it_, params->image_topics[1], 1000, hints);
        sync = new message_filters::TimeSynchronizer<sensor_msgs::Image, sensor_msgs::Image> (*image_sub_l, *image_sub_r, 1000);
        sync->registerCallback(boost::bind(&D2Frontend::stereoImagesCallback, this, _1, _2));
    } else if (params->camera_configuration == CameraConfig::PINHOLE_DEPTH) {
        ROS_INFO("[D2Frontend] Input: raw images %s and depth %s", params->image_topics[0].c_str(), params->depth_topics[0].c_str());
        image_sub_l = new ImageSubscriber(*it_, params->image_topics[0], 1000, hints);
        image_sub_r = new ImageSubscriber(*it_, params->depth_topics[0], 1000, hints);
        sync = new message_filters::TimeSynchronizer<sensor_msgs::Image, sensor_msgs::Image> (*image_sub_l, *image_sub_r, 1000);
        sync->registerCallback(boost::bind(&D2Frontend::depthImagesCallback, this, _1, _2));
    } else if (params->camera_configuration == CameraConfig::FOURCORNER_FISHEYE) {
        //Default we accept only horizon-concated image
        image_sub_single = it_->subscribe(params->image_topics[0], 1000, &D2Frontend::monoImageCallback, this, hints);
    }
    
    keyframe_pub = nh.advertise<swarm_msgs::node_frame>("keyframe", 10);

    loopconn_pub = nh.advertise<swarm_msgs::LoopEdge>("loop", 10);
    
    if (params->enable_sub_remote_frame) {
        ROS_INFO("[SWARM_LOOP] Subscribing remote image from bag");
        remote_img_sub = nh.subscribe("/swarm_loop/remote_frame_desc", 1, &D2Frontend::onRemoteFrameROS, this, ros::TransportHints().tcpNoDelay());
    }

    if (params->enable_pub_remote_frame) {
        remote_image_desc_pub = nh.advertise<swarm_msgs::ImageArrayDescriptor>("remote_frame_desc", 10);
    }

    timer = nh.createTimer(ros::Duration(0.01), [&](const ros::TimerEvent & e) {
        loop_net->scanRecvPackets();
    });

    // loop_timer = nh.createTimer(ros::Duration(0.01), &D2Frontend::loopTimerCallback, this);
    th_loop_det = std::thread(&D2Frontend::loopDetectionThread, this);
    th = std::thread([&] {
        while(0 == loop_net->lcmHandle()) {
        }
    });
}

}
    