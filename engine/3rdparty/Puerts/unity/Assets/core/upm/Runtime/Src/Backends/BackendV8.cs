/*
* Tencent is pleased to support the open source community by making Puerts available.
* Copyright (C) 2020 Tencent.  All rights reserved.
* Puerts is licensed under the BSD 3-Clause License, except for the third-party components listed in the file 'LICENSE' which may be subject to their corresponding license terms. 
* This file is subject to the terms and conditions defined in file 'LICENSE', which is part of this source code package.
*/

using System;


namespace Puerts
{
    public class BackendV8 : BackendJs
    {
        IntPtr isolate;

        public BackendV8(ILoader loader) : base(loader) { }

        public BackendV8() : this(new DefaultLoader())
        {
        }

        public override int GetApiVersion()
        {
            return PapiV8Native.GetV8PapiVersion();
        }

        public override IntPtr CreateEnvRef()
        {
            var envRef = PapiV8Native.CreateV8PapiEnvRef();
            isolate = PapiV8Native.GetV8Isolate(envRef);
            return envRef;
        }

        public override IntPtr GetApi()
        {
            return PapiV8Native.GetV8FFIApi();
        }

        public override void OnTick()
        {
            PapiV8Native.LogicTick(isolate);
        }

        public override void DestroyEnvRef(IntPtr envRef)
        {
            PapiV8Native.DestroyV8PapiEnvRef(envRef);
        }

        public virtual bool IdleNotificationDeadline(double DeadlineInSeconds)
        {
            return PapiV8Native.IdleNotificationDeadline(isolate, DeadlineInSeconds);
        }

        public override void LowMemoryNotification()
        {
            PapiV8Native.LowMemoryNotification(isolate);
        }

        public virtual void RequestMinorGarbageCollectionForTesting()
        {
            PapiV8Native.RequestMinorGarbageCollectionForTesting(isolate);
        }

        public virtual void RequestFullGarbageCollectionForTesting()
        {
            PapiV8Native.RequestFullGarbageCollectionForTesting(isolate);
        }

        public virtual void TerminateExecution()
        {
            PapiV8Native.TerminateExecution(isolate);
        }

        public override void OpenRemoteDebugger(int debugPort)
        {
            PapiV8Native.CreateInspector(isolate, debugPort);
        }

        public override bool DebuggerTick()
        {
            return PapiV8Native.InspectorTick(isolate);
        }

        public override void CloseRemoteDebugger()
        {
            PapiV8Native.DestroyInspector(isolate);
        }
    }
}