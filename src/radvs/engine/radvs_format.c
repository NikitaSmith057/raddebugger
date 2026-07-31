// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#include "radvs/engine/radvs_format.h"

internal void
radvs_event_list_push(RADVS_EventList *list, RADVS_EventNode *node)
{
  node->next = 0;
  SLLQueuePush(list->first, list->last, node);
  list->count += 1;
}

internal void
radvs_event_list_concat(RADVS_EventList *dst, RADVS_EventList *src)
{
  if (src->first) {
    if (dst->last) {
      dst->last->next = src->first;
    } else {
      dst->first = src->first;
    }
    dst->last = src->last;
    dst->count += src->count;
    MemoryZeroStruct(src);
  }
}
