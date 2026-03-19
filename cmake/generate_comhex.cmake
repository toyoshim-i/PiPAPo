# generate_comhex.cmake — Convert a .COM binary to a C header with byte array
#
# Required variables:
#   COM_FILE  — input .COM binary path
#   OUT_FILE  — output .h file path

file(READ "${COM_FILE}" _data HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_data}")
# Wrap lines at 12 bytes for readability
string(REGEX REPLACE "(0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],)" "\\1\n    " _hex "${_hex}")
file(WRITE "${OUT_FILE}"
    "/* Auto-generated from ${COM_FILE} — do not edit */\n"
    "static const unsigned char zexdoc_com[] = {\n    ${_hex}\n};\n"
    "static const unsigned int zexdoc_com_len = sizeof(zexdoc_com);\n")
