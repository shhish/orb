"""Just enough of the protobuf wire format for the provisioning messages, so setup doesn't need protoc output.

Messages are lists of (field number, value): an int is a varint, bytes are length-delimited, a list is a nested
message. As in proto3, zero ints and empty bytes are left out, while a nested message is always written (that is
how a oneof's empty message, such as CmdGetWifiStatus, is sent).
"""

VARINT = 0
FIXED64 = 1
LEN = 2
FIXED32 = 5

Field = tuple[int, "int | bytes | list[Field]"]


class ProtoError(ValueError):
    pass


def _varint(value: int) -> bytes:
    if value < 0:
        value += 1 << 64  # negative int32/int64 are sign-extended to ten bytes
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def encode(fields: list[Field]) -> bytes:
    out = bytearray()
    for number, value in fields:
        if isinstance(value, list):
            data = encode(value)
        elif isinstance(value, int):
            if value:
                out += _varint(number << 3 | VARINT) + _varint(value)
            continue
        else:
            data = bytes(value)
            if not data:
                continue
        out += _varint(number << 3 | LEN) + _varint(len(data)) + data
    return bytes(out)


def _read_varint(data: bytes, pos: int) -> tuple[int, int]:
    value = shift = 0
    while True:
        if pos >= len(data) or shift > 63:
            raise ProtoError("truncated varint")
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, pos


def decode(data: bytes) -> dict[int, list[int | bytes]]:
    """Field number → values in order. Nested messages come back as bytes to decode in turn."""
    fields: dict[int, list[int | bytes]] = {}
    pos = 0
    while pos < len(data):
        key, pos = _read_varint(data, pos)
        number, wire = key >> 3, key & 7
        if wire == VARINT:
            value, pos = _read_varint(data, pos)
        elif wire == LEN:
            length, pos = _read_varint(data, pos)
            value = data[pos : pos + length]
            if len(value) != length:
                raise ProtoError("truncated field")
            pos += length
        elif wire in (FIXED32, FIXED64):
            size = 4 if wire == FIXED32 else 8
            value = data[pos : pos + size]
            if len(value) != size:
                raise ProtoError("truncated field")
            pos += size
        else:
            raise ProtoError(f"unsupported wire type {wire}")
        fields.setdefault(number, []).append(value)
    return fields


def get(fields: dict[int, list[int | bytes]], number: int, default: int | bytes = 0) -> int | bytes:
    """A field's value; the last one wins, as in protobuf."""
    values = fields.get(number)
    return values[-1] if values else default
