
from collections import namedtuple
from struct import unpack

telem_packet_struct_format = "!LdH"

telem_tuple_builder = 'rssi timehms pi status_flags'

TelemPacket = namedtuple('TelemPacket', telem_tuple_builder)


"""Returns TelemPacket object (namedtuple). """
def unpack_telem_packet(data:bytes):
    return TelemPacket._make(unpack(telem_packet_struct_format, data))
