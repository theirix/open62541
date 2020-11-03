/* This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information. */

/**
 * **Trace point setup**
 *
 *            +--------------+                        +----------------+
 *         T1 | OPCUA PubSub |  T8                 T5 | OPCUA loopback |  T4
 *         |  |  Application |  ^                  |  |  Application   |  ^
 *         |  +--------------+  |                  |  +----------------+  |
 *  User   |  |              |  |                  |  |                |  |
 *  Space  |  |              |  |                  |  |                |  |
 *         |  |              |  |                  |  |                |  |
 *------------|--------------|------------------------|----------------|--------
 *         |  |    Node 1    |  |                  |  |     Node 2     |  |
 *  Kernel |  |              |  |                  |  |                |  |
 *  Space  |  |              |  |                  |  |                |  |
 *         |  |              |  |                  |  |                |  |
 *         v  +--------------+  |                  v  +----------------+  |
 *         T2 |  TX tcpdump  |  T7<----------------T6 |   RX tcpdump   |  T3
 *         |  +--------------+                        +----------------+  ^
 *         |                                                              |
 *         ----------------------------------------------------------------
 */

#define _GNU_SOURCE

#include <sched.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/types.h>
#include <sys/io.h>
#include <getopt.h>

/* For thread operations */
#include <pthread.h>

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/plugin/log.h>
#include <open62541/types_generated.h>
#include <open62541/plugin/pubsub_ethernet.h>

#include "ua_pubsub.h"

#ifdef UA_ENABLE_PUBSUB_ETH_UADP_XDP
#include <open62541/plugin/pubsub_ethernet_xdp.h>
#include <linux/if_link.h>
#endif

UA_NodeId readerGroupIdentifier;
UA_NodeId readerIdentifier;

UA_DataSetReaderConfig readerConfig;

/* to find load of each thread
 * ps -L -o pid,pri,%cpu -C pubsub_TSN_publisher */

/* Configurable Parameters */
/* These defines enables the publisher and subscriber of the OPCUA stack */
/* To run only publisher, enable PUBLISHER define alone (comment SUBSCRIBER) */
#define             PUBLISHER
/* To run only subscriber, enable SUBSCRIBER define alone (comment PUBLISHER) */
#define             SUBSCRIBER
/* Cycle time in milliseconds */
#define             DEFAULT_CYCLE_TIME                    0.25
/* Qbv offset */
#define             DEFAULT_QBV_OFFSET                    125
#define             DEFAULT_SOCKET_PRIORITY               3
#if defined(PUBLISHER)
#define             PUBLISHER_ID                          2234
#define             WRITER_GROUP_ID                       101
#define             DATA_SET_WRITER_ID                    62541
#define             DEFAULT_PUBLISHING_MAC_ADDRESS        "opc.eth://01-00-5E-7F-00-01:8.3"
#endif
#if defined(SUBSCRIBER)
#define             PUBLISHER_ID_SUB                      2235
#define             WRITER_GROUP_ID_SUB                   100
#define             DATA_SET_WRITER_ID_SUB                62541
#define             DEFAULT_SUBSCRIBING_MAC_ADDRESS       "opc.eth://01-00-5E-00-00-01:8.3"
#endif
#define             REPEATED_NODECOUNTS                   2    // Default to publish 64 bytes
#define             PORT_NUMBER                           62541
#define             RECEIVE_QUEUE                         2
#define             XDP_FLAG                              XDP_FLAGS_SKB_MODE

/* Non-Configurable Parameters */
/* Milli sec and sec conversion to nano sec */
#define             MILLI_SECONDS                         1000 * 1000
#define             SECONDS                               1000 * 1000 * 1000
#define             SECONDS_SLEEP                         5
/* Publisher will sleep for 60% of cycle time and then prepares the */
/* transmission packet within 40% */
static UA_Double  pubWakeupPercentage     = 0.6;
/* Subscriber will wakeup only during start of cycle and check whether */
/* the packets are received */
static UA_Double  subWakeupPercentage     = 0;
/* User application Pub/Sub will wakeup at the 30% of cycle time and handles the */
/* user data such as read and write in Information model */
static UA_Double  userAppWakeupPercentage = 0.3;
/* Priority of Publisher, Subscriber, User application and server are kept */
/* after some prototyping and analyzing it */
#define             DEFAULT_PUB_SCHED_PRIORITY              78
#define             DEFAULT_SUB_SCHED_PRIORITY              81
#define             DEFAULT_USERAPPLICATION_SCHED_PRIORITY  75
#define             MAX_MEASUREMENTS                        10000000
#define             DEFAULT_PUB_CORE                        2
#define             DEFAULT_SUB_CORE                        2
#define             DEFAULT_USER_APP_CORE                   3
#define             SECONDS_INCREMENT                       1
#ifndef CLOCK_TAI
#define             CLOCK_TAI                               11
#endif
#define             CLOCKID                                 CLOCK_TAI
#define             ETH_TRANSPORT_PROFILE                   "http://opcfoundation.org/UA-Profile/Transport/pubsub-eth-uadp"

/* If the Hardcoded publisher/subscriber MAC addresses need to be changed,
 * change PUBLISHING_MAC_ADDRESS and SUBSCRIBING_MAC_ADDRESS
 */

/* Set server running as true */
UA_Boolean        running           = UA_TRUE;

char*             pubMacAddress     = DEFAULT_PUBLISHING_MAC_ADDRESS;
char*             subMacAddress     = DEFAULT_SUBSCRIBING_MAC_ADDRESS;
static UA_Double  cycleTimeInMsec   = DEFAULT_CYCLE_TIME;
static UA_Int32   socketPriority    = DEFAULT_SOCKET_PRIORITY;
static UA_Int32   pubPriority       = DEFAULT_PUB_SCHED_PRIORITY;
static UA_Int32   subPriority       = DEFAULT_SUB_SCHED_PRIORITY;
static UA_Int32   userAppPriority   = DEFAULT_USERAPPLICATION_SCHED_PRIORITY;
static UA_Int32   pubCore           = DEFAULT_PUB_CORE;
static UA_Int32   subCore           = DEFAULT_SUB_CORE;
static UA_Int32   userAppCore       = DEFAULT_USER_APP_CORE;
static UA_Int32   qbvOffset         = DEFAULT_QBV_OFFSET;
static UA_Boolean disableSoTxtime   = UA_TRUE;
static UA_Boolean enableCsvLog      = UA_FALSE;

/* Variables corresponding to PubSub connection creation,
 * published data set and writer group */
UA_NodeId           connectionIdent;
UA_NodeId           publishedDataSetIdent;
UA_NodeId           writerGroupIdent;
UA_NodeId           pubNodeID;
UA_NodeId           subNodeID;
UA_NodeId           pubRepeatedCountNodeID;
UA_NodeId           subRepeatedCountNodeID;
/* Variables for counter data handling in address space */
UA_UInt64           *pubCounterData = NULL;
UA_DataValue        *pubDataValueRT = NULL;
UA_UInt64           *repeatedCounterData[REPEATED_NODECOUNTS] = {NULL};
UA_DataValue        *repeatedDataValueRT[REPEATED_NODECOUNTS] = {NULL};

UA_UInt64           *subCounterData = NULL;
UA_DataValue        *subDataValueRT = NULL;
UA_UInt64           *subRepeatedCounterData[REPEATED_NODECOUNTS] = {NULL};
UA_DataValue        *subRepeatedDataValueRT[REPEATED_NODECOUNTS] = {NULL};

#if defined(PUBLISHER)
/* File to store the data and timestamps for different traffic */
FILE               *fpPublisher;
char               *filePublishedData      = "publisher_T1.csv";
/* Array to store published counter data */
UA_UInt64           publishCounterValue[MAX_MEASUREMENTS];
size_t              measurementsPublisher  = 0;
/* Array to store timestamp */
struct timespec     publishTimestamp[MAX_MEASUREMENTS];
/* Thread for publisher */
pthread_t           pubthreadID;
struct timespec     dataModificationTime;
#endif

#if defined(SUBSCRIBER)
/* File to store the data and timestamps for different traffic */
FILE               *fpSubscriber;
char               *fileSubscribedData     = "subscriber_T8.csv";
/* Array to store subscribed counter data */
UA_UInt64           subscribeCounterValue[MAX_MEASUREMENTS];
size_t              measurementsSubscriber = 0;
/* Array to store timestamp */
struct timespec     subscribeTimestamp[MAX_MEASUREMENTS];
/* Thread for subscriber */
pthread_t           subthreadID;
/* Variable for PubSub connection creation */
UA_NodeId           connectionIdentSubscriber;
struct timespec     dataReceiveTime;
#endif

/* Thread for user application*/
pthread_t           userApplicationThreadID;

typedef struct {
UA_Server*                   ServerRun;
} serverConfigStruct;

/* Structure to define thread parameters */
typedef struct {
UA_Server*                   server;
void*                        data;
UA_ServerCallback            callback;
UA_Duration                  interval_ms;
UA_UInt64*                   callbackId;
} threadArg;

/* Publisher thread routine for ETF */
void *publisherETF(void *arg);
/* Subscriber thread routine */
void *subscriber(void *arg);
/* User application thread routine */
void *userApplicationPubSub(void *arg);
/* For adding nodes in the server information model */
static void addServerNodes(UA_Server *server);
/* For deleting the nodes created */
static void removeServerNodes(UA_Server *server);
/* To create multi-threads */
static pthread_t threadCreation(UA_Int16 threadPriority, size_t coreAffinity, void *(*thread) (void *),
                                char *applicationName, void *serverConfig);

/* Stop signal */
static void stopHandler(int sign) {
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "received ctrl-c");
    running = UA_FALSE;
}

/**
 * **Nanosecond field handling**
 *
 * Nanosecond field in timespec is checked for overflowing and one second
 * is added to seconds field and nanosecond field is set to zero
*/

static void nanoSecondFieldConversion(struct timespec *timeSpecValue) {
    /* Check if ns field is greater than '1 ns less than 1sec' */
    while (timeSpecValue->tv_nsec > (SECONDS -1)) {
        /* Move to next second and remove it from ns field */
        timeSpecValue->tv_sec  += SECONDS_INCREMENT;
        timeSpecValue->tv_nsec -= SECONDS;
    }

}

/* Add a callback for cyclic repetition */
static UA_StatusCode
addPubSubApplicationCallback(UA_Server *server, UA_NodeId identifier,
                             UA_ServerCallback callback,
                             void *data, UA_Double interval_ms, UA_UInt64 *callbackId) {
    /* Initialize arguments required for the thread to run */
    threadArg *threadArguments = (threadArg *) UA_malloc(sizeof(threadArg));

    /* Pass the value required for the threads */
    threadArguments->server      = server;
    threadArguments->data        = data;
    threadArguments->callback    = callback;
    threadArguments->interval_ms = interval_ms;
    threadArguments->callbackId  = callbackId;

    /* Check the writer group identifier and create the thread accordingly */
    if(UA_NodeId_equal(&identifier, &writerGroupIdent)) {
#if defined(PUBLISHER)
        /* Create the publisher thread with the required priority and core affinity */
        char threadNamePub[10] = "Publisher";
        *callbackId            = threadCreation((UA_Int16)pubPriority, (size_t)pubCore, publisherETF, threadNamePub, threadArguments);
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                    "Publisher thread callback Id: %ld\n", *callbackId);
#endif
    }
    else {
#if defined(SUBSCRIBER)
        /* Create the subscriber thread with the required priority and core affinity */
        char threadNameSub[11] = "Subscriber";
        *callbackId            = threadCreation((UA_Int16)subPriority, (size_t)subCore, subscriber, threadNameSub, threadArguments);
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                    "Subscriber thread callback Id: %ld\n", *callbackId);
#endif
    }

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
changePubSubApplicationCallbackInterval(UA_Server *server, UA_NodeId identifier,
                                        UA_UInt64 callbackId, UA_Double interval_ms) {
    /* Callback interval need not be modified as it is thread based implementation.
     * The thread uses nanosleep for calculating cycle time and modification in
     * nanosleep value changes cycle time */
    return UA_STATUSCODE_GOOD;
}

/* Remove the callback added for cyclic repetition */
static void
removePubSubApplicationCallback(UA_Server *server, UA_NodeId identifier, UA_UInt64 callbackId) {
    if(callbackId && (pthread_join(callbackId, NULL) != 0))
        UA_LOG_WARNING(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                       "Pthread Join Failed thread: %ld\n", callbackId);
}

/* If the external data source is written over the information model, the
 * externalDataWriteCallback will be triggered. The user has to take care and assure
 * that the write leads not to synchronization issues and race conditions. */
static UA_StatusCode
externalDataWriteCallback(UA_Server *server, const UA_NodeId *sessionId,
                          void *sessionContext, const UA_NodeId *nodeId,
                          void *nodeContext, const UA_NumericRange *range,
                          const UA_DataValue *data){
    //node values are updated by using variables in the memory
    //UA_Server_write is not used for updating node values.
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
externalDataReadNotificationCallback(UA_Server *server, const UA_NodeId *sessionId,
                                     void *sessionContext, const UA_NodeId *nodeid,
                                     void *nodeContext, const UA_NumericRange *range){
    //allow read without any preparation
    return UA_STATUSCODE_GOOD;
}

#if defined(SUBSCRIBER)
static void
addPubSubConnectionSubscriber(UA_Server *server, UA_NetworkAddressUrlDataType *networkAddressUrlSubscriber){
    UA_StatusCode    retval                                 = UA_STATUSCODE_GOOD;
    /* Details about the connection configuration and handling are located
     * in the pubsub connection tutorial */
    UA_PubSubConnectionConfig connectionConfig;
    memset(&connectionConfig, 0, sizeof(connectionConfig));
    connectionConfig.name                                   = UA_STRING("Subscriber Connection");
    connectionConfig.enabled                                = UA_TRUE;
#ifdef UA_ENABLE_PUBSUB_ETH_UADP_XDP
    /* Connection options are given as Key/Value Pairs. */
    UA_KeyValuePair connectionOptions[2];
    connectionOptions[0].key                  = UA_QUALIFIEDNAME(0, "xdpflag");
    UA_UInt32 flags                           = XDP_FLAG;
    UA_Variant_setScalar(&connectionOptions[0].value, &flags, &UA_TYPES[UA_TYPES_UINT32]);
    connectionOptions[1].key                  = UA_QUALIFIEDNAME(0, "hwreceivequeue");
    UA_UInt32 rxqueue                         = RECEIVE_QUEUE;
    UA_Variant_setScalar(&connectionOptions[1].value, &rxqueue, &UA_TYPES[UA_TYPES_UINT32]);
    connectionConfig.connectionProperties     = connectionOptions;
    connectionConfig.connectionPropertiesSize = 2;
#endif
    UA_NetworkAddressUrlDataType networkAddressUrlsubscribe = *networkAddressUrlSubscriber;
    connectionConfig.transportProfileUri                    = UA_STRING(ETH_TRANSPORT_PROFILE);
    UA_Variant_setScalar(&connectionConfig.address, &networkAddressUrlsubscribe, &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
    connectionConfig.publisherId.numeric                    = UA_UInt32_random();
    retval |= UA_Server_addPubSubConnection(server, &connectionConfig, &connectionIdentSubscriber);
    if (retval == UA_STATUSCODE_GOOD)
         UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER,"The PubSub Connection was created successfully!");
}

/* Add ReaderGroup to the created connection */
static void
addReaderGroup(UA_Server *server) {
    if (server == NULL)
        return;

    UA_ReaderGroupConfig readerGroupConfig;
    memset (&readerGroupConfig, 0, sizeof(UA_ReaderGroupConfig));
    readerGroupConfig.name    = UA_STRING("ReaderGroup1");
    readerGroupConfig.rtLevel = UA_PUBSUB_RT_FIXED_SIZE;

    readerGroupConfig.subscribingInterval = CYCLE_TIME;
    readerGroupConfig.timeout = 50;  // As we run in 250us cycle time, modify default timeout (1ms) to 50us
    readerGroupConfig.pubsubManagerCallback.addCustomCallback = addPubSubApplicationCallback;
    readerGroupConfig.pubsubManagerCallback.changeCustomCallbackInterval = changePubSubApplicationCallbackInterval;
    readerGroupConfig.pubsubManagerCallback.removeCustomCallback = removePubSubApplicationCallback;

    UA_Server_addReaderGroup(server, connectionIdentSubscriber, &readerGroupConfig,
                             &readerGroupIdentifier);
}

/* Set SubscribedDataSet type to TargetVariables data type
 * Add SubscriberCounter variable to the DataSetReader */
static void addSubscribedVariables (UA_Server *server) {
    UA_Int32 iterator = 0;
    if (server == NULL) {
        return;
    }

    UA_FieldTargetVariable *targetVars = (UA_FieldTargetVariable*) UA_calloc((REPEATED_NODECOUNTS + 1), sizeof(UA_FieldTargetVariable));
    if(!targetVars) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "FieldTargetVariable - Bad out of memory");
        return;
    }

    /* For creating Targetvariable */
    for (iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        subRepeatedCounterData[iterator] = UA_UInt64_new();
        if(!subRepeatedCounterData[iterator]) {
            UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "SubscribeRepeatedCounterData - Bad out of memory");
            return;
        }

        *subRepeatedCounterData[iterator] = 0;
        subRepeatedDataValueRT[iterator] = UA_DataValue_new();
       if(!subRepeatedDataValueRT[iterator]) {
            UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "SubscribeRepeatedCounterDataValue - Bad out of memory");
            return;
        }

        UA_Variant_setScalar(&subRepeatedDataValueRT[iterator]->value, subRepeatedCounterData[iterator], &UA_TYPES[UA_TYPES_UINT64]);
        subRepeatedDataValueRT[iterator]->hasValue = UA_TRUE;

        /* Set the value backend of the above create node to 'external value source' */
        UA_ValueBackend valueBackend;
        valueBackend.backendType = UA_VALUEBACKENDTYPE_EXTERNAL;
        valueBackend.backend.external.value = &subRepeatedDataValueRT[iterator];
        valueBackend.backend.external.callback.userWrite = externalDataWriteCallback;
        valueBackend.backend.external.callback.notificationRead = externalDataReadNotificationCallback;
        UA_Server_setVariableNode_valueBackend(server, UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+50000), valueBackend);

        UA_FieldTargetDataType_init(&targetVars[iterator].targetVariable);
        targetVars[iterator].targetVariable.attributeId  = UA_ATTRIBUTEID_VALUE;
        targetVars[iterator].targetVariable.targetNodeId = UA_NODEID_NUMERIC(1, (UA_UInt32)iterator + 50000);
    }

    subCounterData = UA_UInt64_new();
    if(!subCounterData) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "SubscribeCounterData - Bad out of memory");
        return;
    }

    *subCounterData = 0;
    subDataValueRT = UA_DataValue_new();
    if(!subDataValueRT) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "SubscribeDataValue - Bad out of memory");
        return;
    }

    UA_Variant_setScalar(&subDataValueRT->value, subCounterData, &UA_TYPES[UA_TYPES_UINT64]);
    subDataValueRT->hasValue = UA_TRUE;

    /* Set the value backend of the above create node to 'external value source' */
    UA_ValueBackend valueBackend;
    valueBackend.backendType = UA_VALUEBACKENDTYPE_EXTERNAL;
    valueBackend.backend.external.value = &subDataValueRT;
    valueBackend.backend.external.callback.userWrite = externalDataWriteCallback;
    valueBackend.backend.external.callback.notificationRead = externalDataReadNotificationCallback;
    UA_Server_setVariableNode_valueBackend(server, subNodeID, valueBackend);

    UA_FieldTargetDataType_init(&targetVars[iterator].targetVariable);
    targetVars[iterator].targetVariable.attributeId  = UA_ATTRIBUTEID_VALUE;
    targetVars[iterator].targetVariable.targetNodeId = subNodeID;

    /* Set the subscribed data to TargetVariable type */
    readerConfig.subscribedDataSetType = UA_PUBSUB_SDS_TARGET;
    readerConfig.subscribedDataSet.subscribedDataSetTarget.targetVariables = targetVars;
    readerConfig.subscribedDataSet.subscribedDataSetTarget.targetVariablesSize = REPEATED_NODECOUNTS + 1;
}

/* Add DataSetReader to the ReaderGroup */
static void
addDataSetReader(UA_Server *server) {
    UA_Int32 iterator = 0;
    if (server == NULL) {
        return;
    }

    memset (&readerConfig, 0, sizeof(UA_DataSetReaderConfig));
    readerConfig.name                 = UA_STRING("DataSet Reader 1");
    UA_UInt16 publisherIdentifier     = PUBLISHER_ID_SUB;
    readerConfig.publisherId.type     = &UA_TYPES[UA_TYPES_UINT16];
    readerConfig.publisherId.data     = &publisherIdentifier;
    readerConfig.writerGroupId        = WRITER_GROUP_ID_SUB;
    readerConfig.dataSetWriterId      = DATA_SET_WRITER_ID_SUB;

    readerConfig.messageSettings.encoding = UA_EXTENSIONOBJECT_DECODED;
    readerConfig.messageSettings.content.decoded.type = &UA_TYPES[UA_TYPES_UADPDATASETREADERMESSAGEDATATYPE];
    UA_UadpDataSetReaderMessageDataType *dataSetReaderMessage = UA_UadpDataSetReaderMessageDataType_new();
    dataSetReaderMessage->networkMessageContentMask           = (UA_UadpNetworkMessageContentMask)(UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID |
                                                                 (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_GROUPHEADER |
                                                                 (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_WRITERGROUPID |
                                                                 (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_PAYLOADHEADER);
    readerConfig.messageSettings.content.decoded.data = dataSetReaderMessage;

    /* Setting up Meta data configuration in DataSetReader */
    UA_DataSetMetaDataType *pMetaData = &readerConfig.dataSetMetaData;
    /* FilltestMetadata function in subscriber implementation */
    UA_DataSetMetaDataType_init(pMetaData);
    pMetaData->name                   = UA_STRING ("DataSet Test");
    /* Static definition of number of fields size to 1 to create one
       targetVariable */
    pMetaData->fieldsSize             = REPEATED_NODECOUNTS + 1;
    pMetaData->fields                 = (UA_FieldMetaData*)UA_Array_new (pMetaData->fieldsSize,
                                                                         &UA_TYPES[UA_TYPES_FIELDMETADATA]);

    for (iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_FieldMetaData_init (&pMetaData->fields[iterator]);
        UA_NodeId_copy (&UA_TYPES[UA_TYPES_UINT64].typeId,
                        &pMetaData->fields[iterator].dataType);
        pMetaData->fields[iterator].builtInType = UA_NS0ID_UINT64;
        pMetaData->fields[iterator].valueRank   = -1; /* scalar */
    }

    /* Unsigned Integer DataType */
    UA_FieldMetaData_init (&pMetaData->fields[iterator]);
    UA_NodeId_copy (&UA_TYPES[UA_TYPES_UINT64].typeId,
                    &pMetaData->fields[iterator].dataType);
    pMetaData->fields[iterator].builtInType = UA_NS0ID_UINT64;
    pMetaData->fields[iterator].valueRank   = -1; /* scalar */

    /* Setup Target Variables in DSR config */
    addSubscribedVariables(server);

    /* Setting up Meta data configuration in DataSetReader */
    UA_Server_addDataSetReader(server, readerGroupIdentifier, &readerConfig,
                               &readerIdentifier);

    UA_free(readerConfig.subscribedDataSet.subscribedDataSetTarget.targetVariables);
    UA_free(readerConfig.dataSetMetaData.fields);
    UA_UadpDataSetReaderMessageDataType_delete(dataSetReaderMessage);
}
#endif

#if defined(PUBLISHER)
/**
 * **PubSub connection handling**
 *
 * Create a new ConnectionConfig. The addPubSubConnection function takes the
 * config and creates a new connection. The Connection identifier is
 * copied to the NodeId parameter.
 */
static void
addPubSubConnection(UA_Server *server, UA_NetworkAddressUrlDataType *networkAddressUrlPub){
    /* Details about the connection configuration and handling are located
     * in the pubsub connection tutorial */
    UA_PubSubConnectionConfig connectionConfig;
    memset(&connectionConfig, 0, sizeof(connectionConfig));
    connectionConfig.name                                   = UA_STRING("Publisher Connection");
    connectionConfig.enabled                                = UA_TRUE;
    UA_NetworkAddressUrlDataType networkAddressUrl          = *networkAddressUrlPub;
    connectionConfig.transportProfileUri                    = UA_STRING(ETH_TRANSPORT_PROFILE);
    UA_Variant_setScalar(&connectionConfig.address, &networkAddressUrl,
                         &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
    connectionConfig.publisherId.numeric                    = PUBLISHER_ID;
    /* Connection options are given as Key/Value Pairs - Sockprio and Txtime */
    UA_KeyValuePair connectionOptions[2];
    connectionOptions[0].key = UA_QUALIFIEDNAME(0, "sockpriority");
    UA_Variant_setScalar(&connectionOptions[0].value, &socketPriority, &UA_TYPES[UA_TYPES_UINT32]);
    connectionOptions[1].key = UA_QUALIFIEDNAME(0, "enablesotxtime");
    UA_Variant_setScalar(&connectionOptions[1].value, &disableSoTxtime, &UA_TYPES[UA_TYPES_BOOLEAN]);
    connectionConfig.connectionProperties     = connectionOptions;
    connectionConfig.connectionPropertiesSize = 2;

    UA_Server_addPubSubConnection(server, &connectionConfig, &connectionIdent);
}

/**
 * **PublishedDataSet handling**
 *
 * Details about the connection configuration and handling are located
 * in the pubsub connection tutorial
 */
static void
addPublishedDataSet(UA_Server *server) {
    UA_PublishedDataSetConfig publishedDataSetConfig;
    memset(&publishedDataSetConfig, 0, sizeof(UA_PublishedDataSetConfig));
    publishedDataSetConfig.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
    publishedDataSetConfig.name                 = UA_STRING("Demo PDS");
    UA_Server_addPublishedDataSet(server, &publishedDataSetConfig, &publishedDataSetIdent);
}

/**
 * **DataSetField handling**
 *
 * The DataSetField (DSF) is part of the PDS and describes exactly one
 * published field.
 */
static void
addDataSetField(UA_Server *server) {
    /* Add a field to the previous created PublishedDataSet */
    UA_NodeId dataSetFieldIdentRepeated;
    UA_DataSetFieldConfig dataSetFieldConfig;
#if defined PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
    staticValueSource = UA_DataValue_new();
#endif
    for (UA_Int32 iterator = 0; iterator <  REPEATED_NODECOUNTS; iterator++)
    {
       memset(&dataSetFieldConfig, 0, sizeof(UA_DataSetFieldConfig));

       repeatedCounterData[iterator] = UA_UInt64_new();
       if(!repeatedCounterData[iterator]) {
           UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "PublishRepeatedCounter - Bad out of memory");
           return;
       }

       *repeatedCounterData[iterator] = 0;
       repeatedDataValueRT[iterator] = UA_DataValue_new();
       if(!repeatedDataValueRT[iterator]) {
           UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "PublishRepeatedCounterDataValue - Bad out of memory");
           return;
       }

       UA_Variant_setScalar(&repeatedDataValueRT[iterator]->value, repeatedCounterData[iterator], &UA_TYPES[UA_TYPES_UINT64]);
       repeatedDataValueRT[iterator]->hasValue = UA_TRUE;

       /* Set the value backend of the above create node to 'external value source' */
       UA_ValueBackend valueBackend;
       valueBackend.backendType = UA_VALUEBACKENDTYPE_EXTERNAL;
       valueBackend.backend.external.value = &repeatedDataValueRT[iterator];
       valueBackend.backend.external.callback.userWrite = externalDataWriteCallback;
       valueBackend.backend.external.callback.notificationRead = externalDataReadNotificationCallback;
       UA_Server_setVariableNode_valueBackend(server, UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+10000), valueBackend);

       /* setup RT DataSetField config */
       dataSetFieldConfig.field.variable.rtValueSource.rtInformationModelNode = UA_TRUE;
       dataSetFieldConfig.field.variable.publishParameters.publishedVariable = UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+10000);
       UA_Server_addDataSetField(server, publishedDataSetIdent, &dataSetFieldConfig, &dataSetFieldIdentRepeated);
   }

    UA_NodeId dataSetFieldIdent;
    UA_DataSetFieldConfig dsfConfig;
    memset(&dsfConfig, 0, sizeof(UA_DataSetFieldConfig));

    pubCounterData = UA_UInt64_new();
    if(!pubCounterData) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "PublishCounter - Bad out of memory");
        return;
    }

    *pubCounterData = 0;
    pubDataValueRT = UA_DataValue_new();
    if(!pubDataValueRT) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "PublishDataValue - Bad out of memory");
        return;
    }

    UA_Variant_setScalar(&pubDataValueRT->value, pubCounterData, &UA_TYPES[UA_TYPES_UINT64]);
    pubDataValueRT->hasValue = UA_TRUE;

    /* Set the value backend of the above create node to 'external value source' */
    UA_ValueBackend valueBackend;
    valueBackend.backendType = UA_VALUEBACKENDTYPE_EXTERNAL;
    valueBackend.backend.external.value = &pubDataValueRT;
    valueBackend.backend.external.callback.userWrite = externalDataWriteCallback;
    valueBackend.backend.external.callback.notificationRead = externalDataReadNotificationCallback;
    UA_Server_setVariableNode_valueBackend(server, pubNodeID, valueBackend);

    /* setup RT DataSetField config */
    dsfConfig.field.variable.rtValueSource.rtInformationModelNode = UA_TRUE;
    dsfConfig.field.variable.publishParameters.publishedVariable = pubNodeID;

    UA_Server_addDataSetField(server, publishedDataSetIdent, &dsfConfig, &dataSetFieldIdent);

}

/**
 * **WriterGroup handling**
 *
 * The WriterGroup (WG) is part of the connection and contains the primary
 * configuration parameters for the message creation.
 */
static void
addWriterGroup(UA_Server *server) {
    UA_WriterGroupConfig writerGroupConfig;
    memset(&writerGroupConfig, 0, sizeof(UA_WriterGroupConfig));
    writerGroupConfig.name               = UA_STRING("Demo WriterGroup");
    writerGroupConfig.publishingInterval = cycleTimeInMsec;
    writerGroupConfig.enabled            = UA_FALSE;
    writerGroupConfig.encodingMimeType   = UA_PUBSUB_ENCODING_UADP;
    writerGroupConfig.writerGroupId      = WRITER_GROUP_ID;
    writerGroupConfig.rtLevel            = UA_PUBSUB_RT_FIXED_SIZE;

    writerGroupConfig.pubsubManagerCallback.addCustomCallback = addPubSubApplicationCallback;
    writerGroupConfig.pubsubManagerCallback.changeCustomCallbackInterval = changePubSubApplicationCallbackInterval;
    writerGroupConfig.pubsubManagerCallback.removeCustomCallback = removePubSubApplicationCallback;

    writerGroupConfig.messageSettings.encoding             = UA_EXTENSIONOBJECT_DECODED;
    writerGroupConfig.messageSettings.content.decoded.type = &UA_TYPES[UA_TYPES_UADPWRITERGROUPMESSAGEDATATYPE];
    /* The configuration flags for the messages are encapsulated inside the
     * message- and transport settings extension objects. These extension
     * objects are defined by the standard. e.g.
     * UadpWriterGroupMessageDataType */
    UA_UadpWriterGroupMessageDataType *writerGroupMessage  = UA_UadpWriterGroupMessageDataType_new();
    /* Change message settings of writerGroup to send PublisherId,
     * WriterGroupId in GroupHeader and DataSetWriterId in PayloadHeader
     * of NetworkMessage */
    writerGroupMessage->networkMessageContentMask          = (UA_UadpNetworkMessageContentMask)(UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID |
                                                              (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_GROUPHEADER |
                                                              (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_WRITERGROUPID |
                                                              (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_PAYLOADHEADER);
    writerGroupConfig.messageSettings.content.decoded.data = writerGroupMessage;
    UA_Server_addWriterGroup(server, connectionIdent, &writerGroupConfig, &writerGroupIdent);
    UA_Server_setWriterGroupOperational(server, writerGroupIdent);
    UA_UadpWriterGroupMessageDataType_delete(writerGroupMessage);
}

/**
 * **DataSetWriter handling**
 *
 * A DataSetWriter (DSW) is the glue between the WG and the PDS. The DSW is
 * linked to exactly one PDS and contains additional informations for the
 * message generation.
 */
static void
addDataSetWriter(UA_Server *server) {
    UA_NodeId dataSetWriterIdent;
    UA_DataSetWriterConfig dataSetWriterConfig;
    memset(&dataSetWriterConfig, 0, sizeof(UA_DataSetWriterConfig));
    dataSetWriterConfig.name            = UA_STRING("Demo DataSetWriter");
    dataSetWriterConfig.dataSetWriterId = DATA_SET_WRITER_ID;
    dataSetWriterConfig.keyFrameCount   = 10;
    UA_Server_addDataSetWriter(server, writerGroupIdent, publishedDataSetIdent,
                               &dataSetWriterConfig, &dataSetWriterIdent);
}

/**
 * **Published data handling**
 *
 * The published data is updated in the array using this function
 */
#if defined(PUBLISHER)
static void
updateMeasurementsPublisher(struct timespec start_time,
                            UA_UInt64 counterValue) {
    if(measurementsPublisher >= MAX_MEASUREMENTS) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "Publisher: Maximum log measurements reached - Closing the application");
        running = UA_FALSE;
        return;
    }

    publishTimestamp[measurementsPublisher]        = start_time;
    publishCounterValue[measurementsPublisher]     = counterValue;
    measurementsPublisher++;
}
#endif
#if defined(SUBSCRIBER)
/**
 * Subscribed data handling**
 * The subscribed data is updated in the array using this function Subscribed data handling**
 */
static void
updateMeasurementsSubscriber(struct timespec receive_time,
                             UA_UInt64 counterValue) {
    if(measurementsSubscriber >= MAX_MEASUREMENTS) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "Subscriber: Maximum log measurements reached - Closing the application");
        running = UA_FALSE;
        return;
    }

    subscribeTimestamp[measurementsSubscriber]     = receive_time;
    subscribeCounterValue[measurementsSubscriber]  = counterValue;
    measurementsSubscriber++;
}
#endif

/**
 * **Publisher thread routine**
 *
 * The publisherETF function is the routine used by the publisher thread.
 * This routine publishes the data at a cycle time of 250us.
 */
void *publisherETF(void *arg) {
    struct timespec   nextnanosleeptime;
    UA_ServerCallback pubCallback;
    UA_Server*        server;
    UA_WriterGroup*   currentWriterGroup; // TODO: Remove WriterGroup Usage
    UA_UInt64         interval_ns;
    UA_UInt64         transmission_time;

    /* Initialise value for nextnanosleeptime timespec */
    nextnanosleeptime.tv_nsec                      = 0;

    threadArg *threadArgumentsPublisher = (threadArg *)arg;
    server                              = threadArgumentsPublisher->server;
    pubCallback                         = threadArgumentsPublisher->callback;
    currentWriterGroup                  = (UA_WriterGroup *)threadArgumentsPublisher->data;
    interval_ns                         = (UA_UInt64)(threadArgumentsPublisher->interval_ms * MILLI_SECONDS);

    /* Get current time and compute the next nanosleeptime */
    clock_gettime(CLOCKID, &nextnanosleeptime);
    /* Variable to nano Sleep until 1ms before a 1 second boundary */
    nextnanosleeptime.tv_sec                      += SECONDS_SLEEP;
    nextnanosleeptime.tv_nsec                      = (__syscall_slong_t)(cycleTimeInMsec * MILLI_SECONDS * pubWakeupPercentage);
    nanoSecondFieldConversion(&nextnanosleeptime);

    /* Define Ethernet ETF transport settings */
    UA_EthernetWriterGroupTransportDataType ethernettransportSettings;
    memset(&ethernettransportSettings, 0, sizeof(UA_EthernetWriterGroupTransportDataType));
    /* TODO: Txtime enable shall be configured based on connectionConfig.etfConfiguration.sotxtimeEnabled parameter */
    ethernetETFtransportSettings.txtime_enabled    = disableSoTxtime;
    ethernetETFtransportSettings.transmission_time = 0;

    /* Encapsulate ETF config in transportSettings */
    UA_ExtensionObject transportSettings;
    memset(&transportSettings, 0, sizeof(UA_ExtensionObject));
    /* TODO: transportSettings encoding and type to be defined */
    transportSettings.content.decoded.data       = &ethernettransportSettings;
    currentWriterGroup->config.transportSettings = transportSettings;
    UA_UInt64 roundOffCycleTime                  = (UA_UInt64)((cycleTimeInMsec * MILLI_SECONDS) - (cycleTimeInMsec * MILLI_SECONDS * pubWakeupPercentage));

    while (running) {
        clock_nanosleep(CLOCKID, TIMER_ABSTIME, &nextnanosleeptime, NULL);
        transmission_time                              = ((UA_UInt64)nextnanosleeptime.tv_sec * SECONDS + (UA_UInt64)nextnanosleeptime.tv_nsec) + roundOffCycleTime + (UA_UInt64)(qbvOffset * 1000);
        ethernettransportSettings.transmission_time = transmission_time;
        pubCallback(server, currentWriterGroup);
        nextnanosleeptime.tv_nsec                     += (__syscall_slong_t)interval_ns;
        nanoSecondFieldConversion(&nextnanosleeptime);
    }

    UA_free(threadArgumentsPublisher);

    return (void*)NULL;
}
#endif

#if defined(SUBSCRIBER)
/**
 * **Subscriber thread routine**
 *
 * The subscriber function is the routine used by the subscriber thread.
 */

void *subscriber(void *arg) {
    UA_Server*        server;
    void*             currentReaderGroup;
    UA_ServerCallback subCallback;
    struct timespec   nextnanosleeptimeSub;
    UA_UInt64         subInterval_ns;

    threadArg *threadArgumentsSubscriber = (threadArg *)arg;
    server             = threadArgumentsSubscriber->server;
    subCallback        = threadArgumentsSubscriber->callback;
    currentReaderGroup = threadArgumentsSubscriber->data;
    subInterval_ns     = (UA_UInt64)(threadArgumentsSubscriber->interval_ms * MILLI_SECONDS);

    /* Get current time and compute the next nanosleeptime */
    clock_gettime(CLOCKID, &nextnanosleeptimeSub);
    /* Variable to nano Sleep until 1ms before a 1 second boundary */
    nextnanosleeptimeSub.tv_sec         += SECONDS_SLEEP;
    nextnanosleeptimeSub.tv_nsec         = (__syscall_slong_t)subWakeupPercentage;
    nanoSecondFieldConversion(&nextnanosleeptimeSub);
    while (running) {
        clock_nanosleep(CLOCKID, TIMER_ABSTIME, &nextnanosleeptimeSub, NULL);
        /* Read subscribed data from the SubscriberCounter variable */
        subCallback(server, currentReaderGroup);
        nextnanosleeptimeSub.tv_nsec += (__syscall_slong_t)subInterval_ns;
        nanoSecondFieldConversion(&nextnanosleeptimeSub);
    }

    UA_free(threadArgumentsSubscriber);

    return (void*)NULL;
}
#endif

#if defined(PUBLISHER) || defined(SUBSCRIBER)
/**
 * **UserApplication thread routine**
 *
 */
void *userApplicationPubSub(void *arg) {
    UA_UInt64  repeatedCounterValue = 10;
    struct timespec nextnanosleeptimeUserApplication;
    /* Get current time and compute the next nanosleeptime */
    clock_gettime(CLOCKID, &nextnanosleeptimeUserApplication);
    /* Variable to nano Sleep until 1ms before a 1 second boundary */
    nextnanosleeptimeUserApplication.tv_sec                      += SECONDS_SLEEP;
    nextnanosleeptimeUserApplication.tv_nsec                      = (__syscall_slong_t)(cycleTimeInMsec * MILLI_SECONDS * userAppWakeupPercentage);
    nanoSecondFieldConversion(&nextnanosleeptimeUserApplication);
    *pubCounterData      = 0;
    for (UA_Int32 iterator = 0; iterator <  REPEATED_NODECOUNTS; iterator++)
    {
        *repeatedCounterData[iterator] = repeatedCounterValue;
    }

    while (running) {
        clock_nanosleep(CLOCKID, TIMER_ABSTIME, &nextnanosleeptimeUserApplication, NULL);
#if defined(PUBLISHER)
        *pubCounterData      = *pubCounterData + 1;
        for (UA_Int32 iterator = 0; iterator <  REPEATED_NODECOUNTS; iterator++)
            *repeatedCounterData[iterator] = *repeatedCounterData[iterator] + 1;

        clock_gettime(CLOCKID, &dataModificationTime);
#endif
#if defined(SUBSCRIBER)
        clock_gettime(CLOCKID, &dataReceiveTime);

        if (enableCsvLog) {
#if defined(PUBLISHER)
            updateMeasurementsPublisher(dataModificationTime, *pubCounterData);
#endif
#if defined(SUBSCRIBER)
            if (*subCounterData > 0)
                updateMeasurementsSubscriber(dataReceiveTime, *subCounterData);
#endif
        }

        nextnanosleeptimeUserApplication.tv_nsec += (__syscall_slong_t)(cycleTimeInMsec * MILLI_SECONDS);
        nanoSecondFieldConversion(&nextnanosleeptimeUserApplication);
    }

    return (void*)NULL;
}
#endif
/**
 * **Deletion of nodes**
 *
 * The removeServerNodes function is used to delete the publisher and subscriber
 * nodes.
 */
static void removeServerNodes(UA_Server *server) {
    /* Delete the Publisher Counter Node*/
    UA_Server_deleteNode(server, pubNodeID, UA_TRUE);
    UA_NodeId_clear(&pubNodeID);
    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_Server_deleteNode(server, pubRepeatedCountNodeID, UA_TRUE);
        UA_NodeId_clear(&pubRepeatedCountNodeID);
    }

    UA_Server_deleteNode(server, subNodeID, UA_TRUE);
    UA_NodeId_clear(&subNodeID);
    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_Server_deleteNode(server, subRepeatedCountNodeID, UA_TRUE);
        UA_NodeId_clear(&subRepeatedCountNodeID);
    }
}

static pthread_t threadCreation(UA_Int16 threadPriority, size_t coreAffinity, void *(*thread) (void *), char *applicationName, void *serverConfig){

    /* Core affinity set */
    cpu_set_t           cpuset;
    pthread_t           threadID;
    struct sched_param  schedParam;
    UA_Int32         returnValue         = 0;
    UA_Int32         errorSetAffinity    = 0;
    /* Return the ID for thread */
    threadID = pthread_self();
    schedParam.sched_priority = threadPriority;
    returnValue = pthread_setschedparam(threadID, SCHED_FIFO, &schedParam);
    if (returnValue != 0) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,"pthread_setschedparam: failed\n");
        exit(1);
    }
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,\
                "\npthread_setschedparam:%s Thread priority is %d \n", \
                applicationName, schedParam.sched_priority);
    CPU_ZERO(&cpuset);
    CPU_SET(coreAffinity, &cpuset);
    errorSetAffinity = pthread_setaffinity_np(threadID, sizeof(cpu_set_t), &cpuset);
    if (errorSetAffinity) {
        fprintf(stderr, "pthread_setaffinity_np: %s\n", strerror(errorSetAffinity));
        exit(1);
    }

    returnValue = pthread_create(&threadID, NULL, thread, serverConfig);
    if (returnValue != 0) {
        UA_LOG_WARNING(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,":%s Cannot create thread\n", applicationName);
    }

    if (CPU_ISSET(coreAffinity, &cpuset)) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,"%s CPU CORE: %ld\n", applicationName, coreAffinity);
    }

   return threadID;

}
/**
 * **Creation of nodes**
 *
 * The addServerNodes function is used to create the publisher and subscriber
 * nodes.
 */
static void addServerNodes(UA_Server *server) {
    UA_NodeId objectId;
    UA_NodeId newNodeId;
    UA_ObjectAttributes object           = UA_ObjectAttributes_default;
    object.displayName                   = UA_LOCALIZEDTEXT("en-US", "Counter Object");
    UA_Server_addObjectNode(server, UA_NODEID_NULL,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                            UA_QUALIFIEDNAME(1, "Counter Object"), UA_NODEID_NULL,
                            object, NULL, &objectId);
    UA_VariableAttributes publisherAttr  = UA_VariableAttributes_default;
    UA_UInt64 publishValue               = 0;
    publisherAttr.accessLevel            = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Variant_setScalar(&publisherAttr.value, &publishValue, &UA_TYPES[UA_TYPES_UINT64]);
    publisherAttr.displayName            = UA_LOCALIZEDTEXT("en-US", "Publisher Counter");
    publisherAttr.dataType               = UA_TYPES[UA_TYPES_UINT64].typeId;
    newNodeId                            = UA_NODEID_STRING(1, "PublisherCounter");
    UA_Server_addVariableNode(server, newNodeId, objectId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Publisher Counter"),
                              UA_NODEID_NULL, publisherAttr, NULL, &pubNodeID);
    UA_VariableAttributes subscriberAttr = UA_VariableAttributes_default;
    UA_UInt64 subscribeValue             = 0;
    subscriberAttr.accessLevel           = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Variant_setScalar(&subscriberAttr.value, &subscribeValue, &UA_TYPES[UA_TYPES_UINT64]);
    subscriberAttr.displayName           = UA_LOCALIZEDTEXT("en-US", "Subscriber Counter");
    subscriberAttr.dataType              = UA_TYPES[UA_TYPES_UINT64].typeId;
    newNodeId                            = UA_NODEID_STRING(1, "SubscriberCounter");
    UA_Server_addVariableNode(server, newNodeId, objectId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Subscriber Counter"),
                              UA_NODEID_NULL, subscriberAttr, NULL, &subNodeID);
    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_VariableAttributes repeatedNodePub = UA_VariableAttributes_default;
        UA_UInt64 repeatedPublishValue        = 0;
        repeatedNodePub.accessLevel           = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        UA_Variant_setScalar(&repeatedNodePub.value, &repeatedPublishValue, &UA_TYPES[UA_TYPES_UINT64]);
        repeatedNodePub.displayName           = UA_LOCALIZEDTEXT("en-US", "Publisher RepeatedCounter");
        repeatedNodePub.dataType              = UA_TYPES[UA_TYPES_UINT64].typeId;
        newNodeId                             = UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+10000);
        UA_Server_addVariableNode(server, newNodeId, objectId,
                                 UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                 UA_QUALIFIEDNAME(1, "Publisher RepeatedCounter"),
                                 UA_NODEID_NULL, repeatedNodePub, NULL, &pubRepeatedCountNodeID);
    }

    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_VariableAttributes repeatedNodeSub = UA_VariableAttributes_default;
        UA_UInt64 repeatedSubscribeValue;
        UA_Variant_setScalar(&repeatedNodeSub.value, &repeatedSubscribeValue, &UA_TYPES[UA_TYPES_UINT64]);
        repeatedNodeSub.accessLevel           = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        repeatedNodeSub.displayName           = UA_LOCALIZEDTEXT("en-US", "Subscriber RepeatedCounter");
        repeatedNodeSub.dataType              = UA_TYPES[UA_TYPES_UINT64].typeId;
        newNodeId                             = UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+50000);
        UA_Server_addVariableNode(server, newNodeId, objectId,
                                 UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                 UA_QUALIFIEDNAME(1, "Subscriber RepeatedCounter"),
                                 UA_NODEID_NULL, repeatedNodeSub, NULL, &subRepeatedCountNodeID);
    }

}

static void usage(char *appname)
{
    fprintf(stderr,
        "\n"
        "usage: %s [options]\n"
        "\n"
        " -interface       [name] Use network interface 'name'\n"
        " -cycleTimeInMsec [num]  Cycle time in milli seconds (default %lf)\n"
        " -socketPriority  [num]  Set publisher SO_PRIORITY to (default %d)\n"
        " -pubPriority     [num]  Publisher thread priority value (default %d)\n"
        " -subPriority     [num]  Subscriber thread priority value (default %d)\n"
        " -userAppPriority [num]  User application thread priority value (default %d)\n"
        " -pubCore         [num]  Run on CPU for publisher (default %d)\n"
        " -subCore         [num]  Run on CPU for subscriber (default %d)\n"
        " -userAppCore     [num]  Run on CPU for userApplication (default %d)\n"
        " -pubMacAddress   [name] Publisher Mac address (default %s - where 8 is the VLAN ID and 3 is the PCP)\n"
        " -subMacAddress   [name] Subscriber Mac address (default %s - where 8 is the VLAN ID and 3 is the PCP)\n"
        " -qbvOffset       [num]  QBV offset value (default %d)\n"
        " -disableSoTxtime        Do not use SO_TXTIME\n"
        " -enableCsvLog           To log the data in csv files\n"
        "\n",
        appname, DEFAULT_CYCLE_TIME, DEFAULT_SOCKET_PRIORITY, DEFAULT_PUB_SCHED_PRIORITY, \
        DEFAULT_SUB_SCHED_PRIORITY, DEFAULT_USERAPPLICATION_SCHED_PRIORITY, \
        DEFAULT_PUB_CORE, DEFAULT_SUB_CORE, DEFAULT_USER_APP_CORE, \
        DEFAULT_PUBLISHING_MAC_ADDRESS, DEFAULT_SUBSCRIBING_MAC_ADDRESS, DEFAULT_QBV_OFFSET);
}

/**
 * **Main Server code**
 *
 * The main function contains publisher and subscriber threads running in
 * parallel.
 */
int main(int argc, char **argv) {
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);

    UA_Int32         returnValue         = 0;
    UA_StatusCode    retval              = UA_STATUSCODE_GOOD;
    UA_Server       *server              = UA_Server_new();
    UA_ServerConfig *config              = UA_Server_getConfig(server);
    char            *interface           = NULL;
    UA_Int32         argInputs           = 0;
    UA_Int32         long_index          = 0;
    char            *progname;
    pthread_t        userThreadID;

    /* Process the command line arguments */
    progname = strrchr(argv[0], '/');
    progname = progname ? 1 + progname : argv[0];

    static struct option long_options[] = {
        {"interface",         required_argument, 0, 'a'},
        {"cycleTimeInMsec",   required_argument, 0, 'b'},
        {"socketPriority",    required_argument, 0, 'c'},
        {"pubPriority",       required_argument, 0, 'd'},
        {"subPriority",       required_argument, 0, 'e'},
        {"userAppPriority",   required_argument, 0, 'f'},
        {"pubCore",           required_argument, 0, 'g'},
        {"subCore",           required_argument, 0, 'h'},
        {"userAppCore",       required_argument, 0, 'i'},
        {"pubMacAddress",     required_argument, 0, 'j'},
        {"subMacAddress",     required_argument, 0, 'k'},
        {"qbvOffset",         required_argument, 0, 'l'},
        {"disableSoTxtime",   no_argument,       0, 'm'},
        {"enableCsvLog",      no_argument,       0, 'n'},
        {"help",              no_argument,       0, 'o'},
        {0,                   0,                 0,  0 }
    };

    while ((argInputs = getopt_long_only(argc, argv,"", long_options, &long_index)) != -1) {
        switch (argInputs) {
            case 'a':
                interface = optarg;
                break;
            case 'b':
                cycleTimeInMsec = atof(optarg);
                break;
            case 'c':
                socketPriority = atoi(optarg);
                break;
            case 'd':
                pubPriority = atoi(optarg);
                break;
            case 'e':
                subPriority = atoi(optarg);
                break;
            case 'f':
                userAppPriority = atoi(optarg);
                break;
            case 'g':
                pubCore = atoi(optarg);
                break;
            case 'h':
                subCore = atoi(optarg);
                break;
            case 'i':
                userAppCore = atoi(optarg);
                break;
            case 'j':
                pubMacAddress = optarg;
                break;
            case 'k':
                subMacAddress = optarg;
                break;
            case 'l':
                qbvOffset = atoi(optarg);
                break;
            case 'm':
                disableSoTxtime = UA_FALSE;
                break;
            case 'n':
                enableCsvLog = UA_TRUE;
                break;
            case 'o':
                usage(progname);
                return -1;
        }
    }

    if (!interface) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "Need a network interface to run");
        usage(progname);
        return -1;
    }

    if (cycleTimeInMsec < 0.125) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "%f Bad cycle time", cycleTimeInMsec);
        usage(progname);
        return -1;
    }

    UA_ServerConfig_setMinimal(config, PORT_NUMBER, NULL);

#if defined(PUBLISHER)
    UA_NetworkAddressUrlDataType networkAddressUrlPub;
#endif
#if defined(SUBSCRIBER)
    UA_NetworkAddressUrlDataType networkAddressUrlSub;
#endif

#if defined(PUBLISHER)
        networkAddressUrlPub.networkInterface = UA_STRING(interface);
        networkAddressUrlPub.url              = UA_STRING(pubMacAddress);
#endif
#if defined(SUBSCRIBER)
        networkAddressUrlSub.networkInterface = UA_STRING(interface);
        networkAddressUrlSub.url              = UA_STRING(subMacAddress);
#endif

if (enableCsvLog) {
#if defined(PUBLISHER)
    fpPublisher                   = fopen(filePublishedData, "w");
#endif
#if defined(SUBSCRIBER)
    fpSubscriber                  = fopen(fileSubscribedData, "w");
#endif
}

#if defined(PUBLISHER) && defined(SUBSCRIBER)
/* Details about the connection configuration and handling are located in the pubsub connection tutorial */
    config->pubsubTransportLayers = (UA_PubSubTransportLayer *)
                                    UA_malloc(2 * sizeof(UA_PubSubTransportLayer));
#else
    config->pubsubTransportLayers = (UA_PubSubTransportLayer *)
                                    UA_malloc(sizeof(UA_PubSubTransportLayer));
#endif
    if (!config->pubsubTransportLayers) {
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }

/* It is possible to use multiple PubSubTransportLayers on runtime.
 * The correct factory is selected on runtime by the standard defined
 * PubSub TransportProfileUri's.
*/

#if defined (PUBLISHER)
    config->pubsubTransportLayers[0] = UA_PubSubTransportLayerEthernet();
    config->pubsubTransportLayersSize++;
#endif

    /* Create variable nodes for publisher and subscriber in address space */
    addServerNodes(server);

#if defined(PUBLISHER)
    addPubSubConnection(server, &networkAddressUrlPub);
    addPublishedDataSet(server);
    addDataSetField(server);
    addWriterGroup(server);
    addDataSetWriter(server);
    UA_Server_freezeWriterGroupConfiguration(server, writerGroupIdent);
#endif

#if defined (PUBLISHER) && defined(SUBSCRIBER)
#if defined (UA_ENABLE_PUBSUB_ETH_UADP_XDP)
    config->pubsubTransportLayers[1] = UA_PubSubTransportLayerEthernetXDP();
    config->pubsubTransportLayersSize++;
#else
    config->pubsubTransportLayers[1] = UA_PubSubTransportLayerEthernet();
    config->pubsubTransportLayersSize++;
#endif
#endif

#if defined(SUBSCRIBER) && !defined(PUBLISHER)
#if defined (UA_ENABLE_PUBSUB_ETH_UADP_XDP)
    config->pubsubTransportLayers[0] = UA_PubSubTransportLayerEthernetXDP();
    config->pubsubTransportLayersSize++;
#else
    config->pubsubTransportLayers[0] = UA_PubSubTransportLayerEthernet();
    config->pubsubTransportLayersSize++;
#endif
#endif

#if defined(SUBSCRIBER)
    addPubSubConnectionSubscriber(server, &networkAddressUrlSub);
    addReaderGroup(server);
    addDataSetReader(server);
    UA_Server_freezeReaderGroupConfiguration(server, readerGroupIdentifier);
    UA_Server_setReaderGroupOperational(server, readerGroupIdentifier);
#endif
    serverConfigStruct *serverConfig;
    serverConfig            = (serverConfigStruct*)UA_malloc(sizeof(serverConfigStruct));
    serverConfig->ServerRun = server;

#if defined(PUBLISHER) || defined(SUBSCRIBER)
    char threadNameUserApplication[22] = "UserApplicationPubSub";
    userThreadID                       = threadCreation((UA_Int16)userAppPriority, (size_t)userAppCore, userApplicationPubSub, threadNameUserApplication, serverConfig);
#endif
    retval |= UA_Server_run(server, &running);

    UA_Server_unfreezeReaderGroupConfiguration(server, readerGroupIdentifier);
#if defined(PUBLISHER) || defined(SUBSCRIBER)
    returnValue = pthread_join(userThreadID, NULL);
    if (returnValue != 0) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,"\nPthread Join Failed for User thread:%d\n", returnValue);
    }
#endif

#if defined(PUBLISHER)
if (enableCsvLog) {
    /* Write the published data in the publisher_T1.csv file */
   size_t pubLoopVariable               = 0;
   for (pubLoopVariable = 0; pubLoopVariable < measurementsPublisher;
        pubLoopVariable++) {
        fprintf(fpPublisher, "%ld,%ld.%09ld\n",
                publishCounterValue[pubLoopVariable],
                publishTimestamp[pubLoopVariable].tv_sec,
                publishTimestamp[pubLoopVariable].tv_nsec);
    }
}
#endif
#if defined(SUBSCRIBER)
if (enableCsvLog) {
    /* Write the subscribed data in the subscriber_T8.csv file */
    size_t subLoopVariable               = 0;
    for (subLoopVariable = 0; subLoopVariable < measurementsSubscriber;
         subLoopVariable++) {
        fprintf(fpSubscriber, "%ld,%ld.%09ld\n",
                subscribeCounterValue[subLoopVariable],
                subscribeTimestamp[subLoopVariable].tv_sec,
                subscribeTimestamp[subLoopVariable].tv_nsec);
    }
}
#endif

#if defined(PUBLISHER) || defined(SUBSCRIBER)
    removeServerNodes(server);
    UA_Server_delete(server);
    UA_free(serverConfig);
#endif
#if defined(PUBLISHER)
    UA_free(pubCounterData);
    for (UA_Int32 iterator = 0; iterator <  REPEATED_NODECOUNTS; iterator++)
        UA_free(repeatedCounterData[iterator]);

    /* Free external data source */
    UA_free(pubDataValueRT);
    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
        UA_free(repeatedDataValueRT[iterator]);
if (enableCsvLog)
    fclose(fpPublisher);
#endif

#if defined(SUBSCRIBER)
    UA_free(subCounterData);
    for (UA_Int32 iterator = 0; iterator <  REPEATED_NODECOUNTS; iterator++)
        UA_free(subRepeatedCounterData[iterator]);

    /* Free external data source */
    UA_free(subDataValueRT);
    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
        UA_free(subRepeatedDataValueRT[iterator]);
if (enableCsvLog)
    fclose(fpSubscriber);
#endif
    return (int)retval;
}
