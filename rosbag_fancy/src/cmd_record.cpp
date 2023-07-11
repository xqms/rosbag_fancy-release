// record command
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include <boost/program_options.hpp>

#include <iostream>
#include <rosfmt/full.h>

#include <ros/node_handle.h>

#include <rosbag_fancy_msgs/Status.h>

#include <std_srvs/Trigger.h>

#include "topic_manager.h"
#include "topic_subscriber.h"
#include "ui.h"
#include "message_queue.h"
#include "mem_str.h"
#include "bag_writer.h"


namespace po = boost::program_options;
using namespace rosbag_fancy;
using namespace rosbag_fancy_msgs;

int record(const std::vector<std::string>& options)
{
	po::variables_map vm;

	// Handle CLI arguments
	{
		po::options_description desc("Options");
		desc.add_options()
			("help", "Display this help message")
			("prefix,p", po::value<std::string>()->default_value("bag"), "Prefix for output bag file. The prefix is extended with a timestamp.")
			("output,o", po::value<std::string>()->value_name("FILE"), "Output bag file (overrides --prefix)")
			("topic", po::value<std::vector<std::string>>()->required(), "Topics to record")
			("queue-size", po::value<std::string>()->value_name("SIZE")->default_value("500MB"), "Queue size")
			("delete-old-at", po::value<std::string>()->value_name("SIZE"), "Delete old bags at given size, e.g. 100GB or 1TB")
			("split-bag-size", po::value<std::string>()->value_name("SIZE"), "Bag size for splitting, e.g. 1GB")
			("paused", "Start paused")
			("no-ui", "Disable terminal UI")
			("udp", "Subscribe using UDP transport")
			("bz2", "Enable BZ2 compression")
			("lz4", "Enable LZ2 compression")
		;

		po::positional_options_description p;
		p.add("topic", -1);

		auto usage = [&](){
			std::cout << "Usage: rosbag_fancy record [options] -o <bag file> <topics...>\n\n";
			std::cout << desc << "\n\n";
			std::cout << "Topics may be annotated with a rate limit in Hz, e.g.:\n";
			std::cout << "  rosbag_fancy /camera/image_raw=10.0\n";
			std::cout << "\n";
		};

		try
		{
			po::store(
				po::command_line_parser(options).options(desc).positional(p).run(),
				vm
			);

			if(vm.count("help"))
			{
				usage();
				return 0;
			}

			po::notify(vm);
		}
		catch(po::error& e)
		{
			std::cerr << "Could not parse arguments: " << e.what() << "\n\n";
			usage();
			return 1;
		}
	}

	ros::NodeHandle nh{"~"};

	std::vector<std::string> topics = vm["topic"].as<std::vector<std::string>>();
	std::sort(topics.begin(), topics.end());

	TopicManager topicManager;
	for(auto& topicSpec : topics)
	{
		std::string name = topicSpec;
		float rateLimit = 0.0f;

		auto sepIdx = topicSpec.find('=');

		if(sepIdx != std::string::npos)
		{
			name = topicSpec.substr(0, sepIdx);

			try
			{
				rateLimit = boost::lexical_cast<float>(topicSpec.substr(sepIdx+1));
			}
			catch(boost::bad_lexical_cast&)
			{
				std::cerr << "Bad topic spec: '" << topicSpec << "'\n";
				return 1;
			}
		}

		int flags = 0;
		if(vm.count("udp"))
			flags |= static_cast<int>(Topic::Flag::UDP);

		topicManager.addTopic(name, rateLimit, flags);
	}

	std::uint64_t queueSize = mem_str::stringToMemory(vm["queue-size"].as<std::string>());
	MessageQueue queue{queueSize};

	// Figure out the output file name
	auto namingMode = BagWriter::Naming::Verbatim;
	std::string bagName = "";
	if(vm.count("output"))
	{
		bagName = vm["output"].as<std::string>();
		namingMode = BagWriter::Naming::Verbatim;
	}
	else
	{
		bagName = vm["prefix"].as<std::string>();
		namingMode = BagWriter::Naming::AppendTimestamp;
	}

	std::uint64_t splitBagSizeInBytes = 0;
	if(vm.count("split-bag-size"))
	{
		splitBagSizeInBytes = mem_str::stringToMemory(vm["split-bag-size"].as<std::string>());
	}

	std::uint64_t deleteOldAtInBytes = 0;
	if(vm.count("delete-old-at"))
	{
		deleteOldAtInBytes = mem_str::stringToMemory(vm["delete-old-at"].as<std::string>());
		if(splitBagSizeInBytes != 0 && deleteOldAtInBytes < splitBagSizeInBytes)
		{
			ROSFMT_WARN("Chosen split-bag-size is larger than delete-old-at size!");
		}
	}

	if(!ros::Time::isValid())
	{
		ROSFMT_INFO("Waiting for ros::Time to become valid...");
		ros::Time::waitForValid();
	}

	BagWriter writer{queue, bagName, namingMode, splitBagSizeInBytes, deleteOldAtInBytes};

	if(vm.count("bz2") && vm.count("lz4"))
	{
		fmt::print(stderr, "Options --bz2 and --lz4 are mutually exclusive\n");
		return 1;
	}

	if(vm.count("bz2"))
		writer.setCompression(rosbag::compression::BZ2);
	if(vm.count("lz4"))
		writer.setCompression(rosbag::compression::LZ4);

	auto start = [&](){
		try
		{
			writer.start();
		}
		catch(rosbag::BagException& e)
		{
			ROSFMT_ERROR("Could not open output bag file: {}", e.what());
			return false;
		}
		return true;
	};

	// Start/Stop service calls
	ros::ServiceServer srv_start = nh.advertiseService("start", boost::function<bool(std_srvs::TriggerRequest&, std_srvs::TriggerResponse&)>([&](auto&, auto& resp){
		resp.success = start();
		return true;
	}));
	ros::ServiceServer srv_stop = nh.advertiseService("stop", boost::function<bool(std_srvs::TriggerRequest&, std_srvs::TriggerResponse&)>([&](auto&, auto& resp){
		writer.stop();
		resp.success = true;
		return true;
	}));

	// Status publisher
	ros::Publisher pub_status = nh.advertise<Status>("status", 1);
	ros::SteadyTimer timer_status = nh.createSteadyTimer(ros::WallDuration(0.1), boost::function<void(const ros::SteadyTimerEvent&)>([&](auto&){
		ros::WallTime now = ros::WallTime::now();

		StatusPtr msg = boost::make_shared<Status>();
		msg->header.stamp = ros::Time::now();

		msg->status = writer.running() ? Status::STATUS_RUNNING : Status::STATUS_PAUSED;

		msg->bagfile = writer.bagfileName();

		msg->bytes = writer.sizeInBytes();
		msg->free_bytes = writer.freeSpace();
		msg->bandwidth = 0;

		auto& counts = writer.messageCounts();

		for(auto& topic : topicManager.topics())
		{
			msg->topics.emplace_back();
			auto& topicMsg = msg->topics.back();

			msg->bandwidth += topic.bandwidth;

			topicMsg.name = topic.name;
			topicMsg.publishers = topic.numPublishers;
			topicMsg.bandwidth = topic.bandwidth;
			topicMsg.bytes = topic.totalBytes;
			topicMsg.messages = topic.totalMessages;

			if(topic.id < counts.size())
				topicMsg.messages_in_current_bag = counts[topic.id];

			topicMsg.rate = topic.messageRateAt(now);
		}

		pub_status.publish(msg);
	}));

	// Start recording if --paused is not given
	if(vm.count("paused") == 0)
	{
		if(!start())
			return 1;
	}

	TopicSubscriber subscriber{topicManager, queue};

	std::unique_ptr<UI> ui;

	if(!vm.count("no-ui"))
		ui.reset(new UI{topicManager, queue, writer});

	ros::spin();

	return 0;
}
