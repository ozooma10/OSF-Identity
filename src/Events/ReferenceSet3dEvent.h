#pragma once

#include "Runtime/OverlayRuntime.h"

namespace Events
{
    using ReferenceSet3dEvent = RE::RuntimeComponentDBFactory::ReferenceSet3d;

    class ReferenceSet3dEventHandler : public REX::TSingleton<ReferenceSet3dEventHandler>, public RE::BSTEventSink<ReferenceSet3dEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const ReferenceSet3dEvent& a_event, RE::BSTEventSource<ReferenceSet3dEvent>*) override
        {
            Runtime::GetOverlayRuntime().OnReferenceSet3d(a_event.ref.get());

            return RE::BSEventNotifyControl::kContinue;
        }

    };
}
