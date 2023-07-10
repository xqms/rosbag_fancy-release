// Filtering view on (multiple) bag files
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include "bag_view.h"

#include <rosbag/bag.h>
#include <std_msgs/Header.h>
#include <std_msgs/UInt8.h>
#include "doctest.h"

namespace rosbag_fancy
{

class BagView::Private
{
public:
	void addBag(BagReader* reader, const std::function<bool(const BagReader::Connection&)>& connectionPredicate)
	{
		auto& handle = m_bags.emplace_back();
		handle.reader = reader;
		handle.filtering = true;

		for(auto& conn : reader->connections())
		{
			if(handle.connectionIDs.size() <= conn.first)
				handle.connectionIDs.resize(conn.first+1, false);

			if(connectionPredicate(conn.second))
				handle.connectionIDs[conn.first] = true;
		}
	}

	void addBag(BagReader* reader)
	{
		auto& handle = m_bags.emplace_back();
		handle.reader = reader;
		handle.filtering = false;
	}

	ros::Time startTime() const
	{
		if(m_bags.empty())
			return {};

		ros::Time start = m_bags.front().reader->startTime();
		for(auto& bag : m_bags)
			start = std::min(start, bag.reader->startTime());

		return start;
	}

	ros::Time endTime() const
	{
		ros::Time end;
		for(auto& bag : m_bags)
			end = std::max(end, bag.reader->endTime());

		return end;
	}

private:
	friend class BagView::Iterator::Private;

	struct BagHandle
	{
		BagReader* reader;
		bool filtering;
		std::vector<bool> connectionIDs;
	};
	std::vector<BagHandle> m_bags;
};

class BagView::Iterator::Private
{
public:
	Private(const BagView::Private* view)
	{
		for(auto& handle : view->m_bags)
		{
			auto& state = m_state.emplace_back();
			state.handle = &handle;
			state.it = handle.reader->begin();

			if(handle.filtering)
				state.skipToNext(false);
		}
	}

	Private(const BagView::Private* view, const ros::Time& time)
	{
		for(auto& handle : view->m_bags)
		{
			auto& state = m_state.emplace_back();
			state.handle = &handle;
			state.it = handle.reader->findTime(time);

			if(handle.filtering)
				state.skipToNext(false);
		}
	}

	struct BagState
	{
		const BagView::Private::BagHandle* handle;
		int chunk = -1;
		BagReader::Iterator it;

		void skipToNext(bool advance)
		{
			// We need to skip to the next valid message in this bag.
			auto messageIsInteresting = [&](const BagReader::Message& msg){
				return handle->connectionIDs[msg.connection->id];
			};
			auto connectionIsInteresting = [&](const BagReader::ConnectionInfo& conn){
				return handle->connectionIDs[conn.id];
			};

			if(advance)
				it.advanceWithPredicates(connectionIsInteresting, messageIsInteresting);
			else
				it.findNextWithPredicates(connectionIsInteresting, messageIsInteresting);
		}
	};

	std::vector<BagState> m_state;
	BagState* m_nextBag{};
	MultiBagMessage m_msg;
};

BagView::Iterator::Iterator(const BagView* view)
 : m_d{std::make_shared<Private>(view->m_d.get())}
{
	++(*this);
}

BagView::Iterator::Iterator(const BagView* view, const ros::Time& time)
 : m_d{std::make_shared<Private>(view->m_d.get(), time)}
{
	++(*this);
}

BagView::Iterator::~Iterator()
{}

const BagView::MultiBagMessage& BagView::Iterator::operator*()
{
	if(!m_d)
		throw std::logic_error{"Attempt to dereference invalid BagView::Iterator"};

	return m_d->m_msg;
}

BagView::Iterator& BagView::Iterator::operator++()
{
	if(!m_d)
		return *this;

	if(m_d->m_nextBag)
	{
		auto* bag = m_d->m_nextBag;

		// We need to skip to the next valid message in this bag.
		if(bag->handle->filtering)
			bag->skipToNext(true);
		else
			++bag->it;
	}

	// Figure out the earliest available message from all the bags
	ros::Time earliestStamp;
	m_d->m_nextBag = nullptr;
	std::size_t bagIndex = 0;

	for(std::size_t i = 0; i < m_d->m_state.size(); ++i)
	{
		auto& state = m_d->m_state[i];

		if(state.it == state.handle->reader->end())
			continue;

		if(!m_d->m_nextBag || state.it->stamp < earliestStamp)
		{
			m_d->m_nextBag = &state;
			earliestStamp = state.it->stamp;
			bagIndex = i;
		}
	}

	if(!m_d->m_nextBag)
	{
		// End reached, invalidate
		m_d.reset();
		return *this;
	}

	// Found a message!
	m_d->m_msg.msg = &*m_d->m_nextBag->it;
	m_d->m_msg.bagIndex = bagIndex;

	return *this;
}

bool operator==(const BagView::Iterator& a, const BagView::Iterator& b)
{
	// NOTE: This way view.begin() != view.begin(), but I don't care.
	return a.m_d == b.m_d;
}

bool operator!=(const BagView::Iterator& a, const BagView::Iterator& b)
{
	// NOTE: This way view.begin() != view.begin(), but I don't care.
	return a.m_d != b.m_d;
}

BagView::BagView()
 : m_d{std::make_unique<Private>()}
{}

BagView::~BagView()
{}

void BagView::addBag(BagReader* reader, const std::function<bool(const BagReader::Connection&)>& connectionPredicate)
{
	m_d->addBag(reader, connectionPredicate);
}

void BagView::addBag(BagReader* reader)
{
	m_d->addBag(reader);
}

ros::Time BagView::startTime() const
{
	return m_d->startTime();
}

ros::Time BagView::endTime() const
{
	return m_d->endTime();
}

BagView::Iterator BagView::begin() const
{
	return BagView::Iterator{this};
}

BagView::Iterator BagView::end() const
{
	return {};
}

BagView::Iterator BagView::findTime(const ros::Time& time) const
{
	return BagView::Iterator{this, time};
}


TEST_CASE("BagView: One file")
{
	// Generate a bag file
	char bagfileName[] = "/tmp/rosbag_fancy_test_XXXXXX";
	{
		int fd = mkstemp(bagfileName);
		REQUIRE(fd >= 0);
		close(fd);

		rosbag::Bag bag{bagfileName, rosbag::BagMode::Write};

		{
			std_msgs::Header msg;
			msg.frame_id = "a";
			bag.write("/topicA", ros::Time(1000, 0), msg);
		}
		{
			std_msgs::Header msg;
			msg.frame_id = "b";
			bag.write("/topicB", ros::Time(1001, 0), msg);
		}
		{
			std_msgs::UInt8 msg;
			msg.data = 123;
			bag.write("/topicC", ros::Time(1002, 0), msg);
		}

		bag.close();
	}

	// Open bagfile
	BagReader reader{bagfileName};

	SUBCASE("No selection")
	{
		BagView view;
		view.addBag(&reader);

		auto it = view.begin();

		REQUIRE(it != view.end());
		CHECK(it->msg->connection->topicInBag == "/topicA");

		++it; REQUIRE(it != view.end());
		CHECK(it->msg->connection->topicInBag == "/topicB");

		++it; REQUIRE(it != view.end());
		CHECK(it->msg->connection->topicInBag == "/topicC");

		++it; CHECK(it == view.end());
	}

	SUBCASE("select by topic")
	{
		BagView view;
		view.addBag(&reader, [&](const BagReader::Connection& con){
			return con.topicInBag == "/topicB";
		});

		int num = 0;
		for(auto& pmsg : view)
		{
			CHECK(pmsg.msg->connection->topicInBag == "/topicB");

			auto msg = pmsg.msg->instantiate<std_msgs::Header>();
			REQUIRE(msg);
			CHECK(msg->frame_id == "b");
			num++;
		}

		CHECK(num == 1);
	}

	SUBCASE("select by type")
	{
		BagView view;
		view.addBag(&reader, [&](const BagReader::Connection& con){
			return con.type == "std_msgs/UInt8";
		});

		int num = 0;
		for(auto& pmsg : view)
		{
			CHECK(pmsg.msg->connection->topicInBag == "/topicC");

			auto msg = pmsg.msg->instantiate<std_msgs::UInt8>();
			REQUIRE(msg);
			CHECK(msg->data == 123);
			num++;
		}

		CHECK(num == 1);
	}

	// Remove temp file
	unlink(bagfileName);
}

}
