/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 *
 *    Copyright 2017 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2017 (c) Stefan Profanter, fortiss GmbH
 *    Copyright 2018 (c) Ari Breitkreuz, fortiss GmbH
 *    Copyright 2018 (c) Thomas Stalder, Blue Time Concept SA
 *    Copyright 2018 (c) Fabian Arndt, Root-Core
 */

#include "ua_server_internal.h"
#include "ua_subscription.h"
#include "ua_types_encoding_binary.h"

#ifdef UA_ENABLE_SUBSCRIPTIONS /* conditional compilation */

#define UA_VALUENCODING_MAXSTACK 512

#define ABS_SUBTRACT_TYPE_INDEPENDENT(a,b) ((a)>(b)?(a)-(b):(b)-(a))

static UA_Boolean
outOfDeadBand(const void *data1, const void *data2, const size_t arrayPos,
              const UA_DataType *type, const UA_Double deadbandValue) {
    if(type == &UA_TYPES[UA_TYPES_BOOLEAN]) {
        if(ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Boolean*)data1)[arrayPos],
                                         ((const UA_Boolean*)data2)[arrayPos]) <= deadbandValue)
            return false;
    } else if(type == &UA_TYPES[UA_TYPES_SBYTE]) {
        if(ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_SByte*)data1)[arrayPos],
                                         ((const UA_SByte*)data2)[arrayPos]) <= deadbandValue)
            return false;
    } else if(type == &UA_TYPES[UA_TYPES_BYTE]) {
        if(ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Byte*)data1)[arrayPos],
                                         ((const UA_Byte*)data2)[arrayPos]) <= deadbandValue)
                return false;
    } else if(type == &UA_TYPES[UA_TYPES_INT16]) {
        if(ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Int16*)data1)[arrayPos],
                                         ((const UA_Int16*)data2)[arrayPos]) <= deadbandValue)
            return false;
    } else if(type == &UA_TYPES[UA_TYPES_UINT16]) {
        if(ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_UInt16*)data1)[arrayPos],
                                         ((const UA_UInt16*)data2)[arrayPos]) <= deadbandValue)
            return false;
    } else if(type == &UA_TYPES[UA_TYPES_INT32]) {
        if(ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Int32*)data1)[arrayPos],
                                         ((const UA_Int32*)data2)[arrayPos]) <= deadbandValue)
            return false;
    } else if(type == &UA_TYPES[UA_TYPES_UINT32]) {
        if(ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_UInt32*)data1)[arrayPos],
                                         ((const UA_UInt32*)data2)[arrayPos]) <= deadbandValue)
            return false;
    } else if(type == &UA_TYPES[UA_TYPES_INT64]) {
        if(ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Int64*)data1)[arrayPos],
                                         ((const UA_Int64*)data2)[arrayPos]) <= deadbandValue)
            return false;
    } else if(type == &UA_TYPES[UA_TYPES_UINT64]) {
        if(ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_UInt64*)data1)[arrayPos],
                                         ((const UA_UInt64*)data2)[arrayPos]) <= deadbandValue)
            return false;
    } else if(type == &UA_TYPES[UA_TYPES_FLOAT]) {
        if(ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Float*)data1)[arrayPos],
                                         ((const UA_Float*)data2)[arrayPos]) <= deadbandValue)
            return false;
    } else if(type == &UA_TYPES[UA_TYPES_DOUBLE]) {
        if(ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Double*)data1)[arrayPos],
                                         ((const UA_Double*)data2)[arrayPos]) <= deadbandValue)
            return false;
    }
    return true;
}

static UA_INLINE UA_Boolean
updateNeededForFilteredValue(const UA_Variant *value, const UA_Variant *oldValue,
                             const UA_Double deadbandValue) {
    if(value->arrayLength != oldValue->arrayLength)
        return true;

    if(value->type != oldValue->type)
        return true;

    if (UA_Variant_isScalar(value)) {
        return outOfDeadBand(value->data, oldValue->data, 0, value->type, deadbandValue);
    } else {
        for (size_t i = 0; i < value->arrayLength; ++i) {
            if (outOfDeadBand(value->data, oldValue->data, i, value->type, deadbandValue))
                return true;
        }
    }
    return false;
}

/* When a change is detected, encoding contains the heap-allocated binary encoded value */
static UA_Boolean
detectValueChangeWithFilter(UA_Server *server, UA_Subscription *sub, UA_MonitoredItem *mon,
                            UA_DataValue *value, UA_ByteString *encoding) {
    if(UA_DataType_isNumeric(value->value.type) &&
       (mon->filter.dataChangeFilter.trigger == UA_DATACHANGETRIGGER_STATUSVALUE ||
        mon->filter.dataChangeFilter.trigger == UA_DATACHANGETRIGGER_STATUSVALUETIMESTAMP)) {
        if(mon->filter.dataChangeFilter.deadbandType == UA_DEADBANDTYPE_ABSOLUTE) {
            if(!updateNeededForFilteredValue(&value->value, &mon->lastValue,
                                             mon->filter.dataChangeFilter.deadbandValue))
                return false;
        }
        /* else if (mon->filter.deadbandType == UA_DEADBANDTYPE_PERCENT) {
            // TODO where do this EURange come from ?
            UA_Double deadbandValue = fabs(mon->filter.deadbandValue * (EURange.high-EURange.low));
            if (!updateNeededForFilteredValue(value->value, mon->lastValue, deadbandValue))
                return false;
        }*/
    }

    /* Stack-allocate some memory for the value encoding. We might heap-allocate
     * more memory if needed. This is just enough for scalars and small
     * structures. */
    UA_STACKARRAY(UA_Byte, stackValueEncoding, UA_VALUENCODING_MAXSTACK);
    UA_ByteString valueEncoding;
    valueEncoding.data = stackValueEncoding;
    valueEncoding.length = UA_VALUENCODING_MAXSTACK;

    /* Encode the value */
    UA_Byte *bufPos = valueEncoding.data;
    const UA_Byte *bufEnd = &valueEncoding.data[valueEncoding.length];
    UA_StatusCode retval = UA_encodeBinary(value, &UA_TYPES[UA_TYPES_DATAVALUE],
                                           &bufPos, &bufEnd, NULL, NULL);
    if(retval == UA_STATUSCODE_BADENCODINGERROR) {
        size_t binsize = UA_calcSizeBinary(value, &UA_TYPES[UA_TYPES_DATAVALUE]);
        if(binsize == 0)
            return false;

        if(binsize > UA_VALUENCODING_MAXSTACK) {
            retval = UA_ByteString_allocBuffer(&valueEncoding, binsize);
            if(retval == UA_STATUSCODE_GOOD) {
                bufPos = valueEncoding.data;
                bufEnd = &valueEncoding.data[valueEncoding.length];
                retval = UA_encodeBinary(value, &UA_TYPES[UA_TYPES_DATAVALUE],
                                         &bufPos, &bufEnd, NULL, NULL);
            }
        }
    }

    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING_SESSION(server->config.logger, sub ? sub->session : &server->adminSession,
                               "Subscription %u | MonitoredItem %i | "
                               "Could not encode the value the MonitoredItem with status %s",
                               sub ? sub->subscriptionId : 0, mon->monitoredItemId,
                               UA_StatusCode_name(retval));
        return false;
    }

    /* Has the value changed? */
    valueEncoding.length = (uintptr_t)bufPos - (uintptr_t)valueEncoding.data;
    UA_Boolean changed = (!mon->lastSampledValue.data ||
                          !UA_String_equal(&valueEncoding, &mon->lastSampledValue));

    /* No change */
    if(!changed) {
        if(valueEncoding.data != stackValueEncoding)
            UA_ByteString_deleteMembers(&valueEncoding);
        return false;
    }

    /* Change detected. Copy encoding on the heap if necessary. */
    if(valueEncoding.data == stackValueEncoding) {
        retval = UA_ByteString_copy(&valueEncoding, encoding);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_WARNING_SESSION(server->config.logger, sub ? sub->session : &server->adminSession,
                                   "Subscription %u | MonitoredItem %i | "
                                   "Detected change, but could not allocate memory for the notification"
                                   "with status %s", sub ? sub->subscriptionId : 0,
                                   mon->monitoredItemId, UA_StatusCode_name(retval));
            return false;
        }
        return true;
    }

    *encoding = valueEncoding;
    return true;
}

/* Has this sample changed from the last one? The method may allocate additional
 * space for the encoding buffer. Detect the change in encoding->data. */
static UA_Boolean
detectValueChange(UA_Server *server, UA_MonitoredItem *mon,
                  UA_DataValue value, UA_ByteString *encoding) {
    /* Apply Filter */
    if(mon->filter.dataChangeFilter.trigger == UA_DATACHANGETRIGGER_STATUS)
        value.hasValue = false;

    value.hasServerTimestamp = false;
    value.hasServerPicoseconds = false;
    if(mon->filter.dataChangeFilter.trigger < UA_DATACHANGETRIGGER_STATUSVALUETIMESTAMP) {
        value.hasSourceTimestamp = false;
        value.hasSourcePicoseconds = false;
    }

    /* Detect the value change */
    return detectValueChangeWithFilter(server, mon->subscription, mon, &value, encoding);
}

/* Returns whether the sample was stored in the MonitoredItem */
static UA_Boolean
sampleCallbackWithValue(UA_Server *server, UA_MonitoredItem *monitoredItem,
                        UA_DataValue *value) {
    UA_assert(monitoredItem->monitoredItemType == UA_MONITOREDITEMTYPE_CHANGENOTIFY);
    UA_Subscription *sub = monitoredItem->subscription;

    /* Contains heap-allocated binary encoding of the value if a change was detected */
    UA_ByteString binValueEncoding = UA_BYTESTRING_NULL;

    /* Has the value changed? Allocates memory in binValueEncoding if necessary.
     * value is edited internally so we make a shallow copy. */
    UA_Boolean changed = detectValueChange(server, monitoredItem, *value, &binValueEncoding);
    if(!changed)
        return false;

    UA_Boolean storedValue = false;
    if(sub) {
        /* Allocate a new notification */
        UA_Notification *newNotification = (UA_Notification *)UA_malloc(sizeof(UA_Notification));
        if(!newNotification) {
            UA_LOG_WARNING_SESSION(server->config.logger, sub ? sub->session : &server->adminSession,
                                   "Subscription %u | MonitoredItem %i | "
                                   "Item for the publishing queue could not be allocated",
                                   sub->subscriptionId, monitoredItem->monitoredItemId);
            UA_ByteString_deleteMembers(&binValueEncoding);
            return false;
        }

        if(value->value.storageType == UA_VARIANT_DATA) {
            newNotification->data.value = *value; /* Move the value to the notification */
            storedValue = true;
        } else { /* => (value->value.storageType == UA_VARIANT_DATA_NODELETE) */
            UA_StatusCode retval = UA_DataValue_copy(value, &newNotification->data.value);
            if(retval != UA_STATUSCODE_GOOD) {
                UA_ByteString_deleteMembers(&binValueEncoding);
                UA_free(newNotification);
                return false;
            }
        }

        /* <-- Point of no return --> */

        /* Enqueue the new notification */
        newNotification->mon = monitoredItem;
        UA_Notification_enqueue(server, sub, monitoredItem, newNotification);
    } else {
        /* Call the local callback if not attached to a subscription */
        UA_LocalMonitoredItem *localMon = (UA_LocalMonitoredItem*) monitoredItem;
        void *nodeContext = NULL;
        UA_Server_getNodeContext(server, monitoredItem->monitoredNodeId, &nodeContext);
        localMon->callback.dataChangeCallback(server, monitoredItem->monitoredItemId,
                                              localMon->context,
                                              &monitoredItem->monitoredNodeId,
                                              nodeContext, monitoredItem->attributeId,
                                              value);
    }

    // If someone called UA_Server_deleteMonitoredItem in the user callback,
    // then the monitored item will be deleted soon. So, there is no need to
    // add the lastValue or lastSampledValue to it.
    //
    // If we do so, we will leak
    // the memory of that values, because UA_Server_deleteMonitoredItem
    // already deleted all members and scheduled the monitored item pointer
    // for later delete. In the later delete the monitored item will be deleted
    // and not the members.
    //
    // Also in the later delete, all type information is lost and a deleteMember
    // is not possible.
    //
    // We do detect if the monitored item is already defunct.
    if (!monitoredItem->sampleCallbackIsRegistered) {
        UA_ByteString_deleteMembers(&binValueEncoding);
        return storedValue;
    }

    /* Store the encoding for comparison */
    UA_ByteString_deleteMembers(&monitoredItem->lastSampledValue);
    monitoredItem->lastSampledValue = binValueEncoding;

    /* Store the value for filter comparison (we don't want to decode
     * lastSampledValue in every iteration) */
    if((monitoredItem->filter.dataChangeFilter.deadbandType == UA_DEADBANDTYPE_PERCENT ||
        monitoredItem->filter.dataChangeFilter.deadbandType == UA_DEADBANDTYPE_ABSOLUTE) &&
       (monitoredItem->filter.dataChangeFilter.trigger == UA_DATACHANGETRIGGER_STATUSVALUE ||
        monitoredItem->filter.dataChangeFilter.trigger == UA_DATACHANGETRIGGER_STATUSVALUETIMESTAMP)) {
        UA_Variant_deleteMembers(&monitoredItem->lastValue);
        UA_Variant_copy(&value->value, &monitoredItem->lastValue);
        /* Don't test the return code here. If this fails, lastValue is empty
         * and a notification will be forced for the next deadband comparison. */
    }

    return storedValue;
}

void
UA_MonitoredItem_sampleCallback(UA_Server *server, UA_MonitoredItem *monitoredItem) {
    UA_Subscription *sub = monitoredItem->subscription;
    UA_Session *session = &server->adminSession;
    if(sub)
        session = sub->session;

    if(monitoredItem->monitoredItemType != UA_MONITOREDITEMTYPE_CHANGENOTIFY) {
        UA_LOG_DEBUG_SESSION(server->config.logger, session, "Subscription %u | "
                             "MonitoredItem %i | Not a data change notification",
                             sub ? sub->subscriptionId : 0, monitoredItem->monitoredItemId);
        return;
    }

    /* Get the node */
    const UA_Node *node = UA_Nodestore_get(server, &monitoredItem->monitoredNodeId);

    /* Sample the value. The sample can still point into the node. */
    UA_DataValue value;
    UA_DataValue_init(&value);
    if(node) {
        UA_ReadValueId rvid;
        UA_ReadValueId_init(&rvid);
        rvid.nodeId = monitoredItem->monitoredNodeId;
        rvid.attributeId = monitoredItem->attributeId;
        rvid.indexRange = monitoredItem->indexRange;
        ReadWithNode(node, server, session, monitoredItem->timestampsToReturn, &rvid, &value);
    } else {
        value.hasStatus = true;
        value.status = UA_STATUSCODE_BADNODEIDUNKNOWN;
    }

    /* Operate on the sample */
    UA_Boolean storedValue = sampleCallbackWithValue(server, monitoredItem, &value);

    /* Delete the sample if it was not stored in the MonitoredItem  */
    if(!storedValue)
        UA_DataValue_deleteMembers(&value); /* Does nothing for UA_VARIANT_DATA_NODELETE */
    if(node)
        UA_Nodestore_release(server, node);
}

#endif /* UA_ENABLE_SUBSCRIPTIONS */
