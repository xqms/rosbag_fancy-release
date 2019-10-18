
rosbag_fancy
============

![animation](https://xqms.github.io/rosbag_fancy/anim.svg)

`rosbag_fancy` is a fancy terminal UI frontend for the venerable [rosbag]
tool.

At the moment, only the `record` command is implemented. It offers the following
advantages over plain `rosbag record`:

 * Live display of statistics per topic, such as number of messages, bandwidth,
   dropped messages, etc. Never notice *after* recording that you misspelled a
   topic name!
 * Bash completion for topic names
 * Optional per-topic rate limiting

[rosbag]: http://wiki.ros.org/rosbag
