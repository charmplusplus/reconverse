#ifndef CONV_RDMA_H
#define CONV_RDMA_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>

// stuff from cmirdmautils.h first 
// zerocopy

typedef struct ncpystruct {

    const void *srcPtr;
    char *srcLayerInfo;
    char *srcAck;
    const void *srcRef;
    int srcPe;
    size_t srcSize;
    short int srcLayerSize;
    short int srcAckSize;
    unsigned char srcRegMode;
    unsigned char srcDeregMode;
    unsigned char isSrcRegistered;

    const void *destPtr;
    char *destLayerInfo;
    char *destAck;
    const void *destRef;
    int destPe;
    size_t destSize;
    short int destAckSize;
    short int destLayerSize;
    unsigned char destRegMode;
    unsigned char destDeregMode;
    unsigned char isDestRegistered;
    unsigned char opMode;

    // Variables used for ack handling
    unsigned char ackMode;
    unsigned char freeMe;
    short int ncpyOpInfoSize;
    int rootNode;
    void *refPtr;

} NcpyOperationInfo;

enum ncpyOperationMode {
    CMK_DIRECT_API = 0,
    CMK_EM_API = 1,
    CMK_EM_API_SRC_ACK_INVOKE = 2,
    CMK_EM_API_DEST_ACK_INVOKE = 3,
    CMK_EM_API_REVERSE = 4,
    CMK_BCAST_EM_API = 5,
    CMK_BCAST_EM_API_REVERSE = 6,
    CMK_READONLY_BCAST = 7,
    CMK_ZC_PUP = 8
};

enum cmiZCMsgType {
    CMK_REG_NO_ZC_MSG = 0,
    CMK_ZC_P2P_SEND_MSG = 1,
    CMK_ZC_P2P_RECV_MSG = 2,
    CMK_ZC_SEND_DONE_MSG =
        3, // USED for both ZC_BCAST_SEND_DONE_MSG & ZC_P2P_SEND_DONE_MSG
    CMK_ZC_BCAST_SEND_MSG = 4,
    CMK_ZC_BCAST_RECV_MSG = 5,
    CMK_ZC_BCAST_RECV_DONE_MSG = 6,
    CMK_ZC_BCAST_RECV_ALL_DONE_MSG = 7,
    CMK_ZC_DEVICE_MSG = 8
};

enum ncpyAckMode { CMK_SRC_DEST_ACK = 0, CMK_SRC_ACK = 1, CMK_DEST_ACK = 2 };

enum ncpyFreeNcpyOpInfoMode {
    CMK_FREE_NCPYOPINFO = 0,
    CMK_DONT_FREE_NCPYOPINFO = 1
};

#define CMI_ZC_MSGTYPE(msg) ((CmiMsgHeaderBasic *)msg)->zcMsgType
#define CMI_IS_ZC_P2P(msg)                                                     \
(CMI_ZC_MSGTYPE(msg) == CMK_ZC_P2P_SEND_MSG ||                               \
    CMI_ZC_MSGTYPE(msg) == CMK_ZC_P2P_RECV_MSG)
#define CMI_IS_ZC_BCAST(msg)                                                   \
(CMI_ZC_MSGTYPE(msg) == CMK_ZC_BCAST_SEND_MSG ||                             \
    CMI_ZC_MSGTYPE(msg) == CMK_ZC_BCAST_RECV_MSG)
#define CMI_IS_ZC_RECV(msg)                                                    \
(CMI_ZC_MSGTYPE(msg) == CMK_ZC_P2P_RECV_MSG ||                               \
    CMI_ZC_MSGTYPE(msg) == CMK_ZC_BCAST_RECV_MSG)
#define CMI_IS_ZC(msg) (CMI_IS_ZC_P2P(msg) || CMI_IS_ZC_BCAST(msg))
#define CMI_IS_ZC_DEVICE(msg) (CMI_ZC_MSGTYPE(msg) == CMK_ZC_DEVICE_MSG)


int getNcpyOpInfoTotalSize(
    int srcLayerSize,
    int srcAckSize,
    int destLayerSize,
    int destAckSize);

void setNcpyOpInfo(
    const void *srcPtr,
    char *srcLayerInfo,
    int srcLayerSize,
    char *srcAck,
    int srcAckSize,
    size_t srcSize,
    unsigned short int srcRegMode,
    unsigned short int srcDeregMode,
    unsigned short int isSrcRegistered,
    int srcPe,
    const void *srcRef,
    const void *destPtr,
    char *destLayerInfo,
    int destLayerSize,
    char *destAck,
    int destAckSize,
    size_t destSize,
    unsigned short int destRegMode,
    unsigned short int destDeregMode,
    unsigned short int isDestRegistered,
    int destPe,
    const void *destRef,
    int rootNode,
    NcpyOperationInfo *ncpyOpInfo);

void resetNcpyOpInfoPointers(NcpyOperationInfo *ncpyOpInfo);

void setReverseModeForNcpyOpInfo(NcpyOperationInfo *ncpyOpInfo);



// now stuff from conv-core-rdma


#endif 