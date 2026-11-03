"""Serial open policy shared by orb-logd; importing this module has no IO."""

import termios

import serial


def open_port(port: str) -> serial.Serial:
    """Open without resetting the chip; clear hang-up so closing doesn't either.

    The USB-Serial-JTAG resets the chip while RTS is asserted and DTR isn't.
    macOS asserts both on open, and lowering DTR before RTS (dtr=False,
    rts=False) passes through exactly that state, so leave both lines alone.
    Exclusive locking prevents cooperating owners.
    """
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = 115200
    connection.timeout = 0
    connection.write_timeout = 1
    connection.exclusive = True
    try:
        connection.open()
        attributes = termios.tcgetattr(connection.fileno())
        attributes[2] &= ~termios.HUPCL
        termios.tcsetattr(connection.fileno(), termios.TCSANOW, attributes)
    except BaseException:
        connection.close()
        raise
    return connection
