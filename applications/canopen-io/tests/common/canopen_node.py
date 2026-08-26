"""Shared canopen.Network handle for functional tests."""
import canopen

import config


class NodeHandle:
    def __init__(self):
        self.network = canopen.Network()
        self.network.connect(channel=config.CAN_CHANNEL,
                              interface="socketcan", bitrate=config.BITRATE)
        self.node = self.network.add_node(config.NODE_ID)

    def close(self):
        self.network.disconnect()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
