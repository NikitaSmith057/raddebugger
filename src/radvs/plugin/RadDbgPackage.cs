using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.Win32;
using Microsoft.VisualStudio;

namespace RAD
{
    [Guid(PackageGuidString)]
    [PackageRegistration(UseManagedResourcesOnly = true, AllowsBackgroundLoading = true)]
    [ProvideAutoLoad(UIContextGuids80.SolutionExists, PackageAutoLoadFlags.BackgroundLoad)]
    [ProvideMenuResource("Menus.ctmenu", 1)]                                 // debug engine picker menu
    [ProvideObject(typeof(RadDbgEngine))]                                    // register the engine object type
    [ProvideRadDbgEngine(typeof(RadDbgEngine), RadDbgEngine.EngineIdString)] // register the engine object identifier
    public sealed class RadDbgPackage : AsyncPackage
    {
        public const string PackageGuidString = "78083fc7-5620-4c90-a77f-fdf476429014";

        public IVsRegisterPriorityCommandTarget? commandTargetRegistration;
        public IVsDebugger?                      debugger;
        public IVsDebugger4?                     debugger4;

        private RadCommandHandler? commandHandler;
        private uint               commandTargetCookie;
        private bool               useRadDbgEngine;
        
        internal bool UseRadDbgEngine
        {
            get => this.useRadDbgEngine;

            set
            {
                ThreadHelper.ThrowIfNotOnUIThread();

                // value already set? -> exit
                if (this.useRadDbgEngine == value) { return;  }

                this.useRadDbgEngine = value;
                using (RegistryKey settings = this.UserRegistryRoot.CreateSubKey("RAD"))
                {
                    settings.SetValue("UseRadDbgEngine", value ? 1 : 0, RegistryValueKind.DWord);
                }

                if (value) { this.RegisterDebugCommandTarget();   }
                else       { this.UnregisterDebugCommandTarget(); }
            }
        }

        internal bool IsDebuggerInDesignMode
        {
            get
            {
                ThreadHelper.ThrowIfNotOnUIThread();
                DBGMODE[] mode = new DBGMODE[1];
                ErrorHandler.ThrowOnFailure(this.debugger!.GetMode(mode));
                return mode[0] == DBGMODE.DBGMODE_Design;
            }
        }

        #region Package Members
        protected override async Task InitializeAsync(CancellationToken cancellationToken, IProgress<ServiceProgressData> progress)
        {
            await this.JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);

            using (RegistryKey? settings = this.UserRegistryRoot.OpenSubKey("RAD"))
            {
                this.useRadDbgEngine = settings?.GetValue("UseRadDbgEngine") is int enabled && enabled != 0;
            }

            this.commandHandler = await RadCommandHandler.InitializeAsync(this);

            this.debugger = await this.GetServiceAsync(typeof(SVsShellDebugger)) as IVsDebugger
                ?? throw new InvalidOperationException("Visual Studio's debugger service is unavailable.");
            
            // Query the VS debugger version 4 inferface.
            this.debugger4 = await this.GetServiceAsync(typeof(SVsShellDebugger)) as IVsDebugger4
                ?? throw new InvalidOperationException("Visual Studio's debugger version 4 service is unavailable.");

            this.commandTargetRegistration = await this.GetServiceAsync(typeof(SVsRegisterPriorityCommandTarget)) as IVsRegisterPriorityCommandTarget
                ?? throw new InvalidOperationException("Visual Studio's command routing service is unavailable.");
            
            await this.JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
            
            if (this.useRadDbgEngine)
            {
                this.RegisterDebugCommandTarget();
            }
        }

        private void RegisterDebugCommandTarget()
        {
            ThreadHelper.ThrowIfNotOnUIThread();

            if (this.commandTargetCookie != 0) { return; }

            RadCommandHandler handler = this.commandHandler
                ?? throw new InvalidOperationException("RAD command handler is unavailable.");

            ErrorHandler.ThrowOnFailure(this.commandTargetRegistration!.RegisterPriorityCommandTarget(
                0,
                new RadDbgCommandTarget(this, handler),
                out this.commandTargetCookie));
        }

        private void UnregisterDebugCommandTarget()
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            if (this.commandTargetCookie == 0) { return; }
            this.commandTargetRegistration!.UnregisterPriorityCommandTarget(this.commandTargetCookie);
            this.commandTargetCookie = 0;
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing && this.commandTargetCookie != 0)
            {
                IVsRegisterPriorityCommandTarget? registration = this.commandTargetRegistration;
                uint cookie = this.commandTargetCookie;
                this.commandTargetCookie = 0;
                if (registration != null)
                {
                    this.JoinableTaskFactory.Run(async () =>
                    {
                        await this.JoinableTaskFactory.SwitchToMainThreadAsync();
                        registration.UnregisterPriorityCommandTarget(cookie);
                    });
                }

            }

            base.Dispose(disposing);
        }

        #endregion
    }
}
