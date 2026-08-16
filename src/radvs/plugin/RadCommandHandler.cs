using System;
using System.Collections.Generic;
using System.ComponentModel.Design;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.VCProjectEngine;
using Microsoft.Win32;

namespace RAD
{
    internal sealed class RadCommandHandler
    {
        public const int DebugEngineItemStartId = 0x0100;
        public const int DebugEnginePlaceholderId = 0x00ff;

        public static readonly Guid CommandSet = new Guid("c19e4a06-d36b-48b6-baf7-7e37a824e803");

        private readonly RadDbgPackage       package;
        private readonly IVsMonitorSelection monitorSelection;
        private readonly List<DebugEngine>   discoveredDebugEngines = new();

        private sealed class DebugEngine
        {
            internal DebugEngine(Guid id, string name)
            {
                this.Id = id;
                this.Name = name;
            }

            internal Guid Id { get; }
            internal string Name { get; }
        }

        private sealed class DynamicDebugEngineMenuCommand : OleMenuCommand
        {
            private readonly Predicate<int> matchItem;

            internal DynamicDebugEngineMenuCommand(CommandID commandId, Predicate<int> matchItem, EventHandler invoke, EventHandler beforeQueryStatus)
                : base(invoke, null, beforeQueryStatus, commandId)
            {
                this.matchItem = matchItem;
            }

            public override bool DynamicItemMatch(int commandId)
            {
                if (this.matchItem(commandId))
                {
                    this.MatchedCommandId = commandId;
                    return true;
                }

                this.MatchedCommandId = 0;
                return false;
            }
        }

        private RadCommandHandler(RadDbgPackage         package,
                                  OleMenuCommandService commandService,
                                  IVsMonitorSelection   monitorSelection)
        {
            this.package          = package          ?? throw new ArgumentNullException(nameof(package));
            this.monitorSelection = monitorSelection ?? throw new ArgumentNullException(nameof(monitorSelection));

            var debugEngineMenu = new DynamicDebugEngineMenuCommand(
                new CommandID(CommandSet, DebugEngineItemStartId),
                this.IsDiscoveredDebugEngineItem,
                this.SelectDiscoveredDebugEngine,
                this.UpdateDiscoveredDebugEngineStatus);
            commandService.AddCommand(debugEngineMenu);

            var placeholder = new OleMenuCommand(null, new CommandID(CommandSet, DebugEnginePlaceholderId));
            placeholder.BeforeQueryStatus += this.UpdateDebugEnginePlaceholderStatus;
            commandService.AddCommand(placeholder);
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

        private bool IsDiscoveredDebugEngineItem(int commandId)
        {
            ThreadHelper.ThrowIfNotOnUIThread();

            // Visual Studio starts a new DynamicItemStart query each time this menu opens.
            if (commandId == DebugEngineItemStartId)
            {
                this.DiscoverDebugEngines();
            }

            int itemIndex = commandId - DebugEngineItemStartId;
            return itemIndex >= 0 && itemIndex < this.discoveredDebugEngines.Count;
        }

        private void SelectDiscoveredDebugEngine(object sender, EventArgs e)
        {
            ThreadHelper.ThrowIfNotOnUIThread();

            DynamicDebugEngineMenuCommand command = (DynamicDebugEngineMenuCommand)sender;
            int itemIndex = this.GetDynamicItemIndex(command);
            if ((uint)itemIndex < (uint)this.discoveredDebugEngines.Count)
            {
                this.package.SelectedDebugEngineId = this.discoveredDebugEngines[itemIndex].Id;
            }
        }

        private void UpdateDiscoveredDebugEngineStatus(object sender, EventArgs e)
        {
            DynamicDebugEngineMenuCommand command = (DynamicDebugEngineMenuCommand)sender;
            int itemIndex = this.GetDynamicItemIndex(command);
            if (itemIndex == 0)
            {
                this.DiscoverDebugEngines();
            }
            if ((uint)itemIndex < (uint)this.discoveredDebugEngines.Count)
            {
                DebugEngine engine = this.discoveredDebugEngines[itemIndex];
                command.Visible = true;
                command.Enabled = true;
                command.Checked = this.package.SelectedDebugEngineId == engine.Id;
                command.Text = engine.Name;
            }
            else
            {
                command.Visible = false;
                command.Enabled = false;
            }

            command.MatchedCommandId = 0;
        }

        private void UpdateDebugEnginePlaceholderStatus(object sender, EventArgs e)
        {
            OleMenuCommand command = (OleMenuCommand)sender;
            this.DiscoverDebugEngines();
            command.Enabled = false;
            command.Visible = this.discoveredDebugEngines.Count == 0;
            command.Text = "No debug engines discovered";
        }

        private int GetDynamicItemIndex(DynamicDebugEngineMenuCommand command)
        {
            int commandId = command.MatchedCommandId == 0 ? DebugEngineItemStartId : command.MatchedCommandId;
            return commandId - DebugEngineItemStartId;
        }

        private void DiscoverDebugEngines()
        {
            this.discoveredDebugEngines.Clear();

            Dictionary<Guid, string> engineNames = new();
            this.AddDebugEngines(this.package.ApplicationRegistryRoot, engineNames);
            this.AddDebugEngines(this.package.UserRegistryRoot, engineNames);

            foreach (KeyValuePair<Guid, string> engine in engineNames)
            {
                this.discoveredDebugEngines.Add(new DebugEngine(engine.Key, engine.Value));
            }

            this.discoveredDebugEngines.Sort((left, right) => string.Compare(left.Name, right.Name, StringComparison.OrdinalIgnoreCase));
        }

        private void AddDebugEngines(RegistryKey registryRoot, Dictionary<Guid, string> engineNames)
        {
            using RegistryKey? engines = registryRoot.OpenSubKey(@"AD7Metrics\Engine");
            if (engines == null)
            {
                return;
            }

            foreach (string keyName in engines.GetSubKeyNames())
            {
                if (!Guid.TryParse(keyName, out Guid engineId))
                {
                    continue;
                }

                using RegistryKey? engine = engines.OpenSubKey(keyName);
                string? name = engine?.GetValue("Name") as string ?? engine?.GetValue(string.Empty) as string;
                if (!string.IsNullOrWhiteSpace(name))
                {
                    engineNames[engineId] = name!;
                }
            }
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

        private async Task LaunchTargetAsync(string executablePath, string arguments, string workingDirectory)
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
                guidLaunchDebugEngine = this.package.SelectedDebugEngineId
                    ?? throw new InvalidOperationException("Select a debug engine before launching."),
            };
            
            // Launch the executable using the selected AD7 engine.
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

                // Launch the target using the selected AD7 engine.
                await this.LaunchTargetAsync(launchSettings.ExecutablePath, launchSettings.Arguments, launchSettings.WorkingDirectory);

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
