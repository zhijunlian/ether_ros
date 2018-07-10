#ifndef ETH_OUTPUT_DATA_HANDLER_LIB_H
#define ETH_OUTPUT_DATA_HANDLER_LIB_H

#include "ros/ros.h"
#include "ighm_ros/EthercatRawData.h"
class EthercatOutputDataHandler
{
  private:
    ros::Subscriber data_raw_sub_;
    ros::Publisher data_pub_;

  public:
    void init(ros::NodeHandle &n);
    void raw_data_callback(const ighm_ros::EthercatRawData::ConstPtr &ethercat_data_raw);
};

#endif /* ETH_OUTPUT_DATA_HANDLER_LIB_H */
