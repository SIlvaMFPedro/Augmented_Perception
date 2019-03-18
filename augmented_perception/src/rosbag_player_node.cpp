//
// Created by pedro on 26-02-2019.
//

// System Includes
#include <iostream>
#include <vector>
#include <ros/ros.h>
#include <sys/select.h>
#include <rosbag/player.h>
#include <rosbag/bag_player.h>
#include <rosbag/message_instance.h>
#include <rosbag/view.h>
#include <rosgraph_msgs/Clock.h>
//#include <rws2019_msgs/MakeAPlay.h>
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

namespace rosbag {
    // PlayerOptions
    PlayerOptions::PlayerOptions() :
                   quiet(false),
                   start_paused(false),
                   at_once(false),
                   bag_time(false),
                   bag_time_frequency(0.0),
                   time_scale(1.0),
                   queue_size(0),
                   advertise_sleep(0.2),
                   try_future(false),
                   has_time(false),
                   loop(false),
                   time(0.0f),
                   keep_alive(false),
                   skip_empty(DURATION_MAX){}
    void PlayerOptions::check() {
        if (bags.size() == 0){
            throw Exception("You must specify at least one bag file to play from");
        }
    }

    //Player
    Player::Player(PlayerOptions const &options) :
                    options_(options),
                    paused_(false),
                    terminal_modified_(false) {}
    Player::~Player(){
        foreach(boost::shared_ptr<Bag> bag, bags_) bag->close();
        restoreTerminal();
    }
    void Player::publish() {
        options_.check();
        // Open all the bag files
        foreach(string const &filename, options_.bags){
            ROS_INFO("Opening %s", filename.c_str());
            try{
                boost::shared_ptr<Bag> bag(new Bag);
                bag->open(filename, bagmode::Read);
                bags_.push_back(bag);
            }
            catch(BagUnindexedException ex){
                cerr << "Bag file " << filename << "is unindexed. RUn rosbag reindex. " << endl;
                return;
            }
        }
        setupTerminal();
        if(!node_handle_.ok()){
            return;
        }
        if(!options_.quiet){
            puts("");
        }

        // Publish all messages in the bags
        View full_view;
        foreach(boost::shared_ptr<Bag> bag, bags_) full_view.addQuery(*bag);
        Time initial_time = full_view.getBeginTime();
        initial_time += Duration(options_.time);

        View view;
        TopicQuery topics(options_.topics);
        if(options_.topics.empty()){
            foreach(boost::shared_ptr<Bag> bag, bags_) view.addQuery(*bag, initial_time, TIME_MAX);
        }
        else{
            foreach(boost::shared_ptr<Bag> bag, bags_) view.addQuery(*bag, topics, initial_time, TIME_MAX);
        }

        if(view.size() == 0){
            cerr << "No messages to play on specified topics. Exiting..." << endl;
            shutdown();
            return;
        }

        // Advertise all of our messages
        foreach(const ConnectionInfo *c, view.getConnections()){
            ros::M_string::const_iterator header_iter = c->header->find("callerid");
            string caller_id = (header_iter != c->header->end() ? header_iter->second : string(""));

            string caller_id_topic = caller_id + c->topic;
            map<string, Publisher>::iterator pub_iter = publishers_.find(caller_id_topic);
            if(pub_iter == publishers_.end()){
                AdvertiseOptions opts = createAdvertiseOptions(c, options_.queue_size);
                Publisher pub = node_handle_.advertise(opts);
                publishers_.insert(publishers_.begin(), pair<string, Publisher>(caller_id_topic, pub));
                pub_iter = publishers_.find(caller_id_topic);
            }
        }
        cout << "Waiting " << options_.advertise_sleep.toSec() << " seconds after advertising topics... " << flush;
        options_.advertise_sleep.sleep();
        cout << "Done!" << endl;
        cout << endl << "Hit space to toggle paused, or 's' to step." << endl;
        paused_ = options_.start_paused;

        while(true){
            // Set up our time_translator and publishers
            time_translator_.setTimeScale(options_.time_scale);
            start_time_ = view.begin()->getTime();
            time_translator_.setRealStartTime(start_time_);
            bag_length_ = view.getEndTime() - view.getBeginTime();

            time_publisher_.setTime(start_time_);
            WallTime now_wt = WallTime::now();
            time_translator_.setTranslatedStartTime(Time(now_wt.sec, now_wt.nsec));

            time_publisher_.setTimeScale(options_.time_scale);
            if(options_.bag_time){
                time_publisher_.setPublishFrequency(options_.bag_time_frequency);
            }
            else{
                time_publisher_.setPublishFrequency(-1.0);
            }
            paused_time_ = now_wt;

            // Call do-publish for each message
            foreach(MessageInstance m, view){
                if(!node_handle_.ok()){
                    break;
                }
                doPublish(m);
            }
            if(options_.keep_alive){
                while(node_handle_.ok()) {
                    doKeepAlive();
                }
            }
            if(!node_handle_.ok()){
                cout << endl;
                break;
            }
            if(!options_.loop){
                cout << endl << "Done!" << endl;
                break;
            }
        }
        shutdown();
    }
    void Player::printTime(){
        if(!options_.quiet){
            Time current_time = time_publisher_.getTime();
            Duration d = current_time - start_time_;

            if(paused_){
                printf("\r [PAUSED]   Bag Time: %13.6f   Duration: %.6f / %.6f     \r", time_publisher_.getTime().toSec(), d.toSec(), bag_length_.toSec());
            }
            else{
                printf("\r [RUNNING]  Bag Time: %13.6f   Duration: %.6f / %.6f     \r", time_publisher_.getTime().toSec(), d.toSec(), bag_length_.toSec());
            }
            fflush(stdout);
        }
    }
    void Player::doPublish(MessageInstance const &m) {
        string const &topic = m.getTopic();
        Time const &time = m.getTime();
        string caller_id = m.getCallerId();

        Time translated = time_translator_.translate(time);
        WallTime horizon = WallTime(translated.sec, translated.nsec);

        time_publisher_.setHorizon(time);
        time_publisher_.setWCHorizon(horizon);

        string caller_id_topic = caller_id + topic;

        map<string, Publisher>::iterator pub_iter = publishers_.find(caller_id_topic);
        ROS_ASSERT(pub_iter != publishers_.end());

        // If immediate specified, play immediately
        if (options_.at_once) {
            time_publisher_.stepClock();
            pub_iter->second.publish(m);
            printTime();
            return;
        }

        // If skip_empty is specified, skip this region and shift.
        if (time - time_publisher_.getTime() > options_.skip_empty) {
            time_publisher_.stepClock();

            WallDuration shift = WallTime::now() - horizon;
            time_translator_.shift(Duration(shift.sec, shift.nsec));
            horizon += shift;
            time_publisher_.setWCHorizon(horizon);
            (pub_iter->second).publish(m);
            printTime();
            return;
        }

        while ((paused_ || !time_publisher_.horizonReached()) && node_handle_.ok()) {
            bool charsleftorpaused = true;
            while (charsleftorpaused && node_handle_.ok()) {
                switch (readCharFromStdin()) {
                    case ' ':
                        paused_ = !paused_;
                        if (paused_) {
                            paused_time_ = WallTime::now();
                        } else {
                            WallDuration shift = WallTime::now() - paused_time_;
                            paused_time_ = WallTime::now();

                            time_translator_.shift(ros::Duration(shift.sec, shift.nsec));
                            horizon += shift;
                            time_publisher_.setWCHorizon(horizon);
                        }
                        break;
                    case 's':
                        if (paused_) {
                            time_publisher_.stepClock();

                            WallDuration shift = WallTime::now() - horizon;
                            paused_time_ = WallTime::now();

                            time_translator_.shift(Duration(shift.sec, shift.nsec));

                            horizon += shift;
                            time_publisher_.setWCHorizon(horizon);

                            (pub_iter->second).publish(m);

                            printTime();
                            return;
                        }
                        break;
                    case 'a':
                        if (paused_) {
                            time_publisher_.stepBackClock();

                            WallDuration shift = WallTime::now() - horizon;
                            paused_time_ = WallTime::now();

                            time_translator_.shift(Duration(shift.sec, shift.nsec));

                            horizon -= shift;

                            time_publisher_.setWCHorizon(horizon);

                            (pub_iter->second).publish(m);

                            printTime();
                            return;
                        }
                        break;
                    case EOF:
                        if (paused_) {
                            printTime();
                            time_publisher_.runStalledClock(WallDuration(.1));
                        }
                        else {
                            charsleftorpaused = false;
                        }
                        break;
                    default:
                        break;
                }
            }
            printTime();
            time_publisher_.runClock(WallDuration(.1));
        }
        pub_iter->second.publish(m);
    }
    void Player::doKeepAlive() {
        //Keep pushing yourself out in 10-sec increments (avoids fancy math dealing with the end of time)
        Time const &time = time_publisher_.getTime() + ros::Duration(10.0);

        Time translated = time_translator_.translate(time);
        WallTime horizon = WallTime(translated.sec, translated.nsec);

        time_publisher_.setHorizon(time);
        time_publisher_.setWCHorizon(horizon);

        if (options_.at_once) {
            return;
        }

        while ((paused_ || !time_publisher_.horizonReached()) && node_handle_.ok()) {
            bool charsleftorpaused = true;
            while (charsleftorpaused && node_handle_.ok()) {
                switch (readCharFromStdin()) {
                    case ' ':
                        paused_ = !paused_;
                        if (paused_) {
                            paused_time_ = WallTime::now();
                        } else {
                            WallDuration shift = WallTime::now() - paused_time_;
                            paused_time_ = WallTime::now();

                            time_translator_.shift(Duration(shift.sec, shift.nsec));

                            horizon += shift;
                            time_publisher_.setWCHorizon(horizon);
                        }
                        break;
                    case EOF:
                        if (paused_) {
                            printTime();
                            time_publisher_.runStalledClock(WallDuration(.1));
                        }
                        else {
                            charsleftorpaused = false;
                        }
                        break;
                    default:
                        break;
                }
            }
            printTime();
            time_publisher_.runClock(WallDuration(.1));
        }
    }
    void Player::setupTerminal() {
        if (terminal_modified_) {
            return;
        }
        const int fd = fileno(stdin);
        termios flags;
        tcgetattr(fd, &orig_flags_);
        flags = orig_flags_;
        flags.c_lflag &= ~ICANON;      // set raw (unset canonical modes)
        flags.c_cc[VMIN] = 0;         // i.e. min 1 char for blocking, 0 chars for non-blocking
        flags.c_cc[VTIME] = 0;         // block if waiting for char
        tcsetattr(fd, TCSANOW, &flags);

        FD_ZERO(&stdin_fdset_);
        FD_SET(fd, &stdin_fdset_);
        maxfd_ = fd + 1;

        terminal_modified_ = true;
    }
    void Player::restoreTerminal() {
        if (!terminal_modified_){
            return;
        }
        const int fd = fileno(stdin);
        tcsetattr(fd, TCSANOW, &orig_flags_);

        terminal_modified_ = false;
    }
    int Player::readCharFromStdin() {
        #ifdef __APPLE__
            fd_set testfd;
            FD_COPY(&stdin_fdset_, &testfd);
        #else
            fd_set testfd = stdin_fdset_;
        #endif

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select(maxfd_, &testfd, NULL, NULL, &tv) <= 0) {
            return EOF;
        }
        return getc(stdin);
    }

    // Time Publisher
    TimePublisher::TimePublisher() : time_scale_(1.0) {
        setPublishFrequency(-1.0);
        time_pub_ = node_handle_.advertise<rosgraph_msgs::Clock>("clock", 1);
    }
    void TimePublisher::setPublishFrequency(double publish_frequency) {
        publish_frequency_ = publish_frequency;

        do_publish_ = (publish_frequency > 0.0);

        wall_step_.fromSec(1.0 / publish_frequency);
    }
    void TimePublisher::setTimeScale(double time_scale) {
        time_scale_ = time_scale;
    }
    void TimePublisher::setHorizon(const Time &horizon) {
        horizon_ = horizon;
    }
    void TimePublisher::setWCHorizon(const WallTime &horizon) {
        wc_horizon_ = horizon;
    }
    void TimePublisher::setTime(const Time &time) {
        current_ = time;
    }
    Time const &TimePublisher::getTime() const {
        return current_;
    }
    void TimePublisher::runClock(const WallDuration &duration) {
        if (do_publish_) {
            rosgraph_msgs::Clock pub_msg;

            WallTime t = WallTime::now();
            WallTime done = t + duration;

            while (t < done && t < wc_horizon_) {
                WallDuration leftHorizonWC = wc_horizon_ - t;

                Duration d(leftHorizonWC.sec, leftHorizonWC.nsec);
                d *= time_scale_;

                current_ = horizon_ - d;

                if (current_ >= horizon_) {
                    current_ = horizon_;
                }
                if (t >= next_pub_) {
                    pub_msg.clock = current_;
                    time_pub_.publish(pub_msg);
                    next_pub_ = t + wall_step_;
                }

                WallTime target = done;
                if (target > wc_horizon_) {
                    target = wc_horizon_;
                }
                if (target > next_pub_) {
                    target = next_pub_;
                }

                WallTime::sleepUntil(target);

                t = WallTime::now();
            }
        }
        else {

            WallTime t = WallTime::now();

            WallDuration leftHorizonWC = wc_horizon_ - t;

            Duration d(leftHorizonWC.sec, leftHorizonWC.nsec);
            d *= time_scale_;

            current_ = horizon_ - d;

            if (current_ >= horizon_) {
                current_ = horizon_;
            }

            WallTime target = WallTime::now() + duration;

            if (target > wc_horizon_) {
                target = wc_horizon_;
            }

            WallTime::sleepUntil(target);
        }
    }
    void TimePublisher::stepClock() {
        if (do_publish_) {
            current_ = horizon_;

            rosgraph_msgs::Clock pub_msg;

            pub_msg.clock = current_;
            time_pub_.publish(pub_msg);

            WallTime t = WallTime::now();
            next_pub_ = t + wall_step_;
        } else {
            current_ = horizon_;
        }
    }
    // edit /opt/ros/melodic/include/rosbag/player.h file and add void method stepBackClock();
    void TimePublisher::stepBackClock() {
        if(do_publish_){
            WallTime t = WallTime::now();
            current_ = horizon_;

            rosgraph_msgs::Clock pub_msg;
            pub_msg.clock = current_;
            time_pub_.publish(pub_msg);
            next_pub_ = t - wall_step_;
        }
        else{
            current_ = horizon_;
        }
    }
    void TimePublisher::runStalledClock(const WallDuration &duration) {
        if(do_publish_){
            rosgraph_msgs::Clock pub_msg;

            WallTime t = WallTime::now();
            WallTime done = t + duration;

            while(t < done){
                if (t > next_pub_){
                    pub_msg.clock = current_;
                    time_pub_.publish(pub_msg);
                    next_pub_ = t + wall_step_;
                }
                WallTime target = done;
                if (target > next_pub_){
                   target = next_pub_;
                }
                WallTime::sleepUntil(target);
                t = WallTime::now();
            }
        }
        else{
            duration.sleep();
        }
    }
    bool TimePublisher::horizonReached() {
        return WallTime::now() > wc_horizon_;
    }
} //namespace rosbag

rosbag::PlayerOptions parseOptions(int argc, char **argv) {
    rosbag::PlayerOptions opts;

    po::options_description desc("Allowed options");

    desc.add_options()
            ("help,h", "produce help message")
            ("quiet,q", "suppress console output")
            ("immediate,i", "play back all messages without waiting")
            ("pause", "start in paused mode")
            ("queue", po::value<int>()->default_value(100), "use an outgoing queue of size SIZE")
            ("clock", "publish the clock time")
            ("hz", po::value<float>()->default_value(100.0), "use a frequency of HZ when publishing clock time")
            ("delay,d", po::value<float>()->default_value(0.2), "sleep SEC seconds after every advertise call")
            ("rate,r", po::value<float>()->default_value(1.0), "multiply the publish rate by FACTOR")
            ("start,s", po::value<float>()->default_value(0.0), "start SEC seconds into the bag files")
            ("loop,l", "loop playback")
            ("keep-alive,k", "keep alive past end of bag")
            ("try-future-version",
             "still try to open a bag file, even if the version is not known to the player")
            ("skip-empty", po::value<float>(),
             "skip regions in the bag with no messages for more than SEC seconds")
            ("topics", po::value<std::vector<std::string> >()->multitoken(), "topics to play back")
            ("bags", po::value<std::vector<std::string> >(), "bag files to play back from");

    po::positional_options_description p;
    p.add("bags", -1);

    po::variables_map vm;

    try {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
    } catch (boost::program_options::invalid_command_line_syntax &e) {
        throw ros::Exception(e.what());
    } catch (boost::program_options::unknown_option &e) {
        throw ros::Exception(e.what());
    }

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        exit(0);
    }

    if (vm.count("quiet"))
        opts.quiet = true;
    if (vm.count("immediate"))
        opts.at_once = true;
    if (vm.count("pause"))
        opts.start_paused = true;
    if (vm.count("queue"))
        opts.queue_size = vm["queue"].as<int>();
    if (vm.count("hz"))
        opts.bag_time_frequency = vm["hz"].as<float>();
    if (vm.count("clock"))
        opts.bag_time = true;
    if (vm.count("delay"))
        opts.advertise_sleep = ros::WallDuration(vm["delay"].as<float>());
    if (vm.count("rate"))
        opts.time_scale = vm["rate"].as<float>();
    if (vm.count("start")) {
        opts.time = vm["start"].as<float>();
        opts.has_time = true;
    }
    if (vm.count("skip-empty"))
        opts.skip_empty = ros::Duration(vm["skip-empty"].as<float>());
    if (vm.count("loop"))
        opts.loop = true;
    if (vm.count("keep-alive"))
        opts.keep_alive = true;

    if (vm.count("topics")) {
        std::vector<std::string> topics = vm["topics"].as<std::vector<std::string> >();
        for (std::vector<std::string>::iterator i = topics.begin();
             i != topics.end();
             i++)
            opts.topics.push_back(*i);
    }

    if (vm.count("bags")) {
        std::vector<std::string> bags = vm["bags"].as<std::vector<std::string> >();
        for (std::vector<std::string>::iterator i = bags.begin();
             i != bags.end();
             i++)
            opts.bags.push_back(*i);
    } else {
        if (vm.count("topics"))
            throw ros::Exception("When using --topics, --bags should be specified to list bags.");
        throw ros::Exception("You must specify at least one bag to play back.");
    }

    return opts;
}

int main(int argc, char **argv){
    //Initialize ROS
    init(argc, argv, "rosbag_player_node");
    NodeHandle nh;

//    rosbag::Bag bag;
//    bag.open("test.bag");  // BagMode is Read by default
//
//    for(rosbag::MessageInstance const m: rosbag::View(bag)){
//        std_msgs::Int32::ConstPtr i = m.instantiate<std_msgs::Int32>();
//        if (i != NULL) {
//            std::cout << i->data << std::endl;
//        }
//    }
//    bag.close();
//    //Spin
//    spin();
    // Parse the command-line options
    rosbag::PlayerOptions opts;
    try{
        opts = parseOptions(argc, argv);
    }
    catch(Exception const &ex){
        ROS_ERROR("Error reading command options: %s", ex.what());
        return 1;
    }

    rosbag::Player player(opts);

    try{
        player.publish();
    }
    catch(std::runtime_error &e){
        ROS_FATAL("%s", e.what());
        return 1;
    }
    spin(); //SPIN

}

