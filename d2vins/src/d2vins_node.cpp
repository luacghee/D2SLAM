#include <d2frontend/d2frontend.h>
#include <d2frontend/d2frontend_types.h>
#include "sensor_msgs/Imu.h"
#include "estimator/d2estimator.hpp"

#define BACKWARD_HAS_DW 1
#include <backward.hpp>
namespace backward
{
    backward::SignalHandling sh;
}

using namespace D2VINS;
class D2VINSNode :  public D2FrontEnd::D2Frontend
{
    D2Estimator estimator;
    ros::Subscriber imu_sub;

protected:
    virtual void frameCallback(const D2FrontEnd::VisualImageDescArray & viokf) override {
        D2FrontEnd::VisualImageDescArray _viokf = viokf; //Here we do not need to copy desc.
        estimator.inputImage(_viokf);
    };

    virtual void imu_callback(const sensor_msgs::Imu & imu) {
        IMUData data(imu);
        data.dt = 1.0/params->IMU_FREQ; //TODO
        estimator.inputImu(data);
    }

public:
    D2VINSNode(ros::NodeHandle & nh) {
        imu_sub  = nh.subscribe("imu", 1, &D2VINSNode::imu_callback, this, ros::TransportHints().tcpNoDelay());
        Init(nh);
        initParams(nh);
    }
    
};

int main(int argc, char **argv)
{
    cv::setNumThreads(1);
    ros::init(argc, argv, "d2vins");
    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);

    D2VINSNode d2vins(n);
    ros::MultiThreadedSpinner spinner(3);
    spinner.spin();
    return 0;
}

