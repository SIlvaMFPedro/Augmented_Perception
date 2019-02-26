//
// Created by pedro on 26-02-2019.
//
#include <iostream>
#include <vector>
#include <ros/ros.h>
#include <sys/select.h>
#include <rosbag/player.h>
#include <rosbag/bag_player.h>
#include <rosbag/message_instance.h>
#include <rosbag/view.h>
#include <rosgraph_msgs/Clock.h>
#include <rws2019_msgs/MakeAPlay.h>
#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>
#include <visualization_msgs/Marker.h>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/program_options.hpp>
#include <std_msgs/Int32.h>

using namespace std;
using namespace boost;
using namespace ros;

#define foreach BOOST_FOREACH

namespace po = program_options;

int main(int argc, char **argv){
    //Initialize ROS
    init(argc, argv, "rosbag_player_node");
    NodeHandle nh;

    rosbag::Bag bag;
    bag.open("test.bag");  // BagMode is Read by default

    for(rosbag::MessageInstance const m: rosbag::View(bag)){
        std_msgs::Int32::ConstPtr i = m.instantiate<std_msgs::Int32>();
        if (i != NULL) {
            std::cout << i->data << std::endl;
        }
    }
    bag.close();
    //Spin
    spin();

}

