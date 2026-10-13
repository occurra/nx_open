// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QSharedPointer>

/**
 * \file
 *
 * This header file contains common typedefs for different resource types.
 *
 * It is to be included from headers that use these typedefs in declarations,
 * but don't need the definitions of the actual resource classes.
 */

class QnResource;
using QnResourcePtr = QSharedPointer<QnResource>;

class QnStorageResource;
using QnStorageResourcePtr = QSharedPointer<QnStorageResource>;
