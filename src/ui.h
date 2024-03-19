// Terminal UI
// Author: Max Schwarz <max.schwarz@uni-bonn.de>

#ifndef ROSBAG_FANCY_UI_H
#define ROSBAG_FANCY_UI_H

#include "terminal.h"
#include "topic_manager.h"
#include "message_queue.h"

#include <ros/steady_timer.h>

#include <boost/signals2.hpp>

namespace rosbag_fancy
{

class BagWriter;
class BagReader;

class UI
{
public:
	UI(TopicManager& config, MessageQueue& queue, BagWriter& writer);

	void draw();

private:
	template<class... Args>
	void printLine(unsigned int& lineCounter, const Args& ... args);

	TopicManager& m_topicManager;
	MessageQueue& m_queue;
	BagWriter& m_bagWriter;

	Terminal m_term;

	ros::SteadyTimer m_timer;
	ros::WallTime m_lastDrawTime;
};

class PlaybackUI
{
public:
	explicit PlaybackUI(TopicManager& topics, const ros::Time& startTime, const ros::Time& endTime);

	void setPositionInBag(const ros::Time& stamp);
	void setPaused(bool paused);

	void draw();

	void handleInput();

	boost::signals2::signal<void()> seekForwardRequested;
	boost::signals2::signal<void()> seekBackwardRequested;
	boost::signals2::signal<void()> pauseRequested;
	boost::signals2::signal<void()> exitRequested;

private:
	template<class... Args>
	void printLine(unsigned int& lineCounter, const Args& ... args);

	TopicManager& m_topicManager;

	ros::Time m_startTime;
	ros::Time m_endTime;

	Terminal m_term;

	ros::SteadyTimer m_timer;
	ros::WallTime m_lastDrawTime;

	ros::Time m_positionInBag;

	ros::SteadyTime m_lastSeekFwd;
	ros::SteadyTime m_lastSeekBwd;

	bool m_paused = false;
};

}

#endif
