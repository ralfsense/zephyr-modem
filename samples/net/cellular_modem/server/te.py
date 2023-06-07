# Copyright (c) 2023, Bjarki Arge Andreasen
# SPDX-License-Identifier: Apache-2.0

import signal
from te_udp_echo import TEUDPEcho

udp_echo = TEUDPEcho()

udp_echo.start()

print("started")

def terminate_handler(a, b):
    udp_echo.stop()
    print("stopped")

signal.signal(signal.SIGTERM, terminate_handler)
signal.signal(signal.SIGINT, terminate_handler)
