/*
 * Copyright (2022) Bytedance Ltd. and/or its affiliates
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <functional>
#include <Common/ProfileEvents.h>
#include <Common/ProfileEventsTimer.h>

namespace ProfileEvents
{
    extern const Event CatalogRequest;
    extern const Event CatalogElapsedMicroseconds;
}

namespace DB
{

namespace Catalog
{
    using Job = std::function<void()>;

    static void runWithMetricSupport(const Job & job, const ProfileEvents::Event & success, const ProfileEvents::Event & failed)
    {
        auto helper = ProfileEventsTimer(ProfileEvents::CatalogRequest, ProfileEvents::CatalogElapsedMicroseconds);
        try
        {
            job();
            ProfileEvents::increment(success);
        }
        catch (...)
        {
            ProfileEvents::increment(failed);
            throw;
        }
    }
}
}
