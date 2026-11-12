/* Result codes shared by the timing component. */
#ifndef TLOG_RESULT_H
#define TLOG_RESULT_H

enum {
    TLOG_OK = 0,
    TLOG_ERR_ARG = -1,
    TLOG_ERR_NOSPACE = -2,
    TLOG_ERR_IO = -4,
    TLOG_ERR_FORMAT = -5,
    TLOG_ERR_VERSION = -6,
    TLOG_ERR_CRC = -7,
};

#endif
