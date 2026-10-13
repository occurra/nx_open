// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "resource.h"

QnResource::QnResource():
    m_mutex(nx::Mutex::Recursive)
{
}

QnResource::~QnResource() = default;

void QnResource::setIdUnsafe(const nx::Uuid& id)
{
    m_id = id;
}
