using System;
using System.ComponentModel.Design;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.VCProjectEngine;

namespace RAD
{
    internal sealed class RadCommandHandler
    {
        public const int NativeCommandId = 0x0100;
        public const int RadCommandId    = 0x0101;

        public static readonly Guid CommandSet = new Guid("c19e4a06-d36b-48b6-baf7-7e37a824e803");

        private readonly RadDbgPackage          package;
        private readonly IVsMonitorSelection monitorSelection;

        private RadCommandHandler(RadDbgPackage         package,
                                  OleMenuCommandService commandService,
                                  IVsMonitorSelection   monitorSelection)
        {
            this.package          = package          ?? throw new ArgumentNullException(nameof(package));
            this.monitorSelection = monitorSelection ?? throw new ArgumentNullException(nameof(monitorSelection));

            // Register RAD Debugger menu item.
            var radCommand = new OleMenuCommand(this.SelectRadDbgEngine, new CommandID(CommandSet, RadCommandId));
            commandService.AddCommand(radCommand);
            radCommand.BeforeQueryStatus += this.UpdateRadDbgEngineStatus;

            // Register the stock native debugger menu item.
            commandService = commandService ?? throw new ArgumentNullException(nameof(commandService));
            var nativeCommand = new OleMenuCommand(this.SelectNativeDebugEngine, new CommandID(CommandSet, NativeCommandId));
            commandService.AddCommand(nativeCommand);
            nativeCommand.BeforeQueryStatus += this.UpdateNativeDebugEngineStatus;
        }

        public static async Task<RadCommandHandler> InitializeAsync(RadDbgPackage package)
        {
            // The constructor registers commands with Visual Studio and must run on the UI thread.
            await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync(package.DisposalToken);

            OleMenuCommandService commandService = await package.GetServiceAsync(typeof(IMenuCommandService)) as OleMenuCommandService
                ?? throw new InvalidOperationException("Visual Studio's command service is unavailable.");
            IVsMonitorSelection monitorSelection = await package.GetServiceAsync(typeof(SVsShellMonitorSelection)) as IVsMonitorSelection
                ?? throw new InvalidOperationException("Visual Studio's property browser is unavailable.");

            return new RadCommandHandler(package, commandService, monitorSelection);
        }

        private void SelectNativeDebugEngine(object sender, EventArgs e)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            this.package.UseRadDbgEngine = false;
        }

        private void SelectRadDbgEngine(object sender, EventArgs e)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            this.package.UseRadDbgEngine = true;
        }

        private void UpdateNativeDebugEngineStatus(object sender, EventArgs e)
        {
            ((OleMenuCommand)sender).Checked = !this.package.UseRadDbgEngine;
        }

        private void UpdateRadDbgEngineStatus(object sender, EventArgs e)
        {
            ((OleMenuCommand)sender).Checked = this.package.UseRadDbgEngine;
        }

        private (string ExecutablePath, string Arguments, string WorkingDirectory) GetActiveCppLaunchSettings()
        {
            ThreadHelper.ThrowIfNotOnUIThread();

            IntPtr hierarchyPtr          = IntPtr.Zero;
            IntPtr selectionContainerPtr = IntPtr.Zero;
            try
            {
                // Get the project hierarchy containing the current selection.
                ErrorHandler.ThrowOnFailure(
                    this.monitorSelection.GetCurrentSelection(out hierarchyPtr, out uint _, out IVsMultiItemSelect _, out selectionContainerPtr));
                if (hierarchyPtr == IntPtr.Zero)
                {
                    throw new InvalidOperationException("Select a single C++ project in Solution Explorer.");
                }

                object hierarchyObject = Marshal.GetObjectForIUnknown(hierarchyPtr);
                if (hierarchyObject is not IVsHierarchy hierarchy)
                {
                    throw new InvalidOperationException("The current selection does not belong to a project.");
                }

                ErrorHandler.ThrowOnFailure(
                    hierarchy.GetProperty(VSConstants.VSITEMID_ROOT, (int)__VSHPROPID.VSHPROPID_ExtObject, out object projectObject));
                if (projectObject is not EnvDTE.Project automationProject || automationProject.Object is not VCProject project)
                {
                    throw new InvalidOperationException("Select a C++ project in Solution Explorer");
                }

                VCConfiguration config = project.ActiveConfiguration ?? throw new InvalidOperationException("The selected C++ project has no active configuration.");
                if (config.DebugSettings is not VCDebugSettings debugSettings)
                {
                    throw new InvalidOperationException("The active C++ configuration has no debug settings.");
                }

                // TODO: attach
                if (debugSettings.Attach)
                {
                    throw new InvalidOperationException("RAD project launch does not support the C++ Attach setting yet.");
                }

                string executablePath   = config.Evaluate(debugSettings.Command);
                string arguments        = config.Evaluate(debugSettings.CommandArguments);
                string workingDirectory = config.Evaluate(debugSettings.WorkingDirectory);

                if (string.IsNullOrWhiteSpace(executablePath))
                {
                    throw new InvalidOperationException("The selected C++ project has no EXE path");
                }

                // Emtpy directory fallback: infer directory from the EXE path.
                if (string.IsNullOrWhiteSpace(workingDirectory))
                {
                    workingDirectory = Path.GetDirectoryName(executablePath) ?? string.Empty;
                }

                return (executablePath, arguments, workingDirectory);
            }
            finally
            {
                if (hierarchyPtr          != IntPtr.Zero) { Marshal.Release(hierarchyPtr);          }
                if (selectionContainerPtr != IntPtr.Zero) { Marshal.Release(selectionContainerPtr); }
            }
        }

        private async Task LaunchRadDbggerTargetAsync(string executablePath, string arguments, string workingDirectory)
        {
            await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync(this.package.DisposalToken);

            // Set up the debug target.
            VsDebugTargetInfo4 target = new()
            {
                dlo                   = (uint)DEBUG_LAUNCH_OPERATION.DLO_CreateProcess,
                LaunchFlags           = (uint)__VSDBGLAUNCHFLAGS.DBGLAUNCH_StopDebuggingOnEnd,
                bstrExe               = executablePath,
                bstrArg               = arguments,
                bstrCurDir            = workingDirectory,
                guidLaunchDebugEngine = new Guid(RadDbgEngine.EngineIdString),
            };
            
            // Launch the executable using the RAD debug engine.
            this.package.debugger4?.LaunchDebugTargets4(1, [target], new VsDebugTargetProcessInfo[1]);
        }

        internal async Task<bool> DebugActiveCppProjectAsync(bool showErrors)
        {
            try
            {
                // Switch to the UI thread.
                await this.package.JoinableTaskFactory.SwitchToMainThreadAsync();
                
                // Read the active C++ project's launch settings.
                var launchSettings = this.GetActiveCppLaunchSettings();

                // Launch the target using the RAD debug engine.
                await this.LaunchRadDbggerTargetAsync(launchSettings.ExecutablePath, launchSettings.Arguments, launchSettings.WorkingDirectory);

                return true;
            }
            catch (OperationCanceledException)
            {
                // Launch was canceled.
                return false;
            }
            catch (Exception exception)
            {
                // Report exception and return.

                ActivityLog.LogError("RAD Debug Engine", exception.ToString());

                if (showErrors)
                {
                    await this.package.JoinableTaskFactory.SwitchToMainThreadAsync();

                    VsShellUtilities.ShowMessageBox(
                        this.package,
                        exception.Message,
                        "RAD Debug Engine",
                        OLEMSGICON.OLEMSGICON_WARNING,
                        OLEMSGBUTTON.OLEMSGBUTTON_OK,
                        OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
                }

                return false;
            }
        }
    }
}
