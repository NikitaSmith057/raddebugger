using System;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.OLE.Interop;
using Microsoft.VisualStudio.Shell;

namespace RAD
{
    internal sealed class RadDbgCommandTarget : IOleCommandTarget
    {
        private readonly RadDbgPackage        package;
        private readonly RadCommandHandler commandHandler;

        public RadDbgCommandTarget(RadDbgPackage package, RadCommandHandler commandHandler)
        {
            this.package        = package        ?? throw new ArgumentNullException(nameof(package));
            this.commandHandler = commandHandler ?? throw new ArgumentNullException(nameof(commandHandler));
        }

        public int Exec(ref Guid commandGroup, uint commandId, uint commandOptions, IntPtr input, IntPtr output)
        {
            ThreadHelper.ThrowIfNotOnUIThread();

            if (this.package.UseRadDbgEngine      &&
                this.package.IsDebuggerInDesignMode &&
                commandGroup == VSConstants.GUID_VSStandardCommandSet97)
            {
                if (commandId == (uint)VSConstants.VSStd97CmdID.Start    ||
                    commandId == (uint)VSConstants.VSStd97CmdID.StepInto ||
                    commandId == (uint)VSConstants.VSStd97CmdID.StepOver ||
                    commandId == (uint)VSConstants.VSStd97CmdID.StepOut  ||
                    commandId == (uint)VSConstants.VSStd97CmdID.RunToCursor)
                {
                    if (this.package.JoinableTaskFactory.Run(() => this.commandHandler.DebugActiveCppProjectAsync(showErrors: true)))
                    {
                        return VSConstants.S_OK;
                    }
                }
            }

            return (int)Constants.OLECMDERR_E_NOTSUPPORTED;
        }

        public int QueryStatus(ref Guid commandGroup, uint commandCount, OLECMD[] commands, IntPtr commandText)
        {
            return (int)Constants.OLECMDERR_E_NOTSUPPORTED;
        }
    }
}
