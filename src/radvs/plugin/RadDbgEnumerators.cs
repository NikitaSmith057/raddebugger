using System;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    internal sealed class RadDbgProcessEnumerator : IEnumDebugProcesses2
    {
        private readonly IDebugProcess2[] processes;
        private uint position;

        internal RadDbgProcessEnumerator(IDebugProcess2 process)
            : this(new[] { process }, 0)
        { 
        }

        internal RadDbgProcessEnumerator(IDebugProcess2[] processes, uint position)
        {
            this.processes = processes;
            this.position = position;
        }

        public int Next(uint celt, IDebugProcess2[] rgelt, ref uint pceltFetched)
        {
            pceltFetched = 0;
            if (rgelt == null)
            {
                return RadDbgHResult.E_POINTER;
            }
            if (celt > (uint)rgelt.Length)
            {
                return RadDbgHResult.E_INVALIDARG;
            }

            while (pceltFetched < celt && this.position < (uint)this.processes.Length)
            {
                rgelt[pceltFetched] = this.processes[this.position];
                ++pceltFetched;
                ++this.position;
            }
            return pceltFetched == celt ? RadDbgHResult.S_OK : RadDbgHResult.S_FALSE;
        }

        public int Skip(uint celt)
        {
            uint remaining = (uint)this.processes.Length - this.position;
            uint skipped = Math.Min(celt, remaining);
            this.position += skipped;
            return skipped == celt ? RadDbgHResult.S_OK : RadDbgHResult.S_FALSE;
        }

        public int Reset()
        {
            this.position = 0;
            return RadDbgHResult.S_OK;
        }

        public int Clone(out IEnumDebugProcesses2 ppEnum)
        {
            ppEnum = new RadDbgProcessEnumerator(this.processes, this.position);
            return RadDbgHResult.S_OK;
        }

        public int GetCount(out uint pcelt)
        {
            pcelt = (uint)this.processes.Length;
            return RadDbgHResult.S_OK;
        }

    }

    internal sealed class RadDbgProgramEnumerator : IEnumDebugPrograms2
    {
        private readonly IDebugProgram2[] programs;
        private uint position;

        internal RadDbgProgramEnumerator(IDebugProgram2 program)
            : this(new[] { program }, 0)
        {
        }

        private RadDbgProgramEnumerator(IDebugProgram2[] programs, uint position)
        {
            this.programs = programs;
            this.position = position;
        }

        public int Next(uint celt, IDebugProgram2[] rgelt, ref uint pceltFetched)
        {
            pceltFetched = 0;
            if (rgelt == null)
            {
                return RadDbgHResult.E_POINTER;
            }
            if (celt > (uint)rgelt.Length)
            {
                return RadDbgHResult.E_INVALIDARG;
            }

            while (pceltFetched < celt && this.position < (uint)this.programs.Length)
            {
                rgelt[pceltFetched] = this.programs[this.position];
                ++pceltFetched;
                ++this.position;
            }
            return pceltFetched == celt ? RadDbgHResult.S_OK : RadDbgHResult.S_FALSE;
        }

        public int Skip(uint celt)
        {
            uint remaining = (uint)this.programs.Length - this.position;
            uint skipped = Math.Min(celt, remaining);
            this.position += skipped;
            return skipped == celt ? RadDbgHResult.S_OK : RadDbgHResult.S_FALSE;
        }

        public int Reset()
        {
            this.position = 0;
            return RadDbgHResult.S_OK;
        }

        public int Clone(out IEnumDebugPrograms2 ppEnum)
        {
            ppEnum = new RadDbgProgramEnumerator(this.programs, this.position);
            return RadDbgHResult.S_OK;
        }

        public int GetCount(out uint pcelt)
        {
            pcelt = (uint)this.programs.Length;
            return RadDbgHResult.S_OK;
        }
    }

    internal sealed class RadDbgThreadEnumerator : IEnumDebugThreads2
    {
        private readonly IDebugThread2[] threads;
        private uint position;

        internal RadDbgThreadEnumerator(IDebugThread2[] threads)
            : this(threads, 0)
        {
        }

        private RadDbgThreadEnumerator(IDebugThread2[] threads, uint position)
        {
            this.threads = threads;
            this.position = position;
        }

        public int Next(uint celt, IDebugThread2[] rgelt, ref uint pceltFetched)
        {
            pceltFetched = 0;
            if (rgelt == null)
            {
                return RadDbgHResult.E_POINTER;
            }
            if (celt > (uint)rgelt.Length)
            {
                return RadDbgHResult.E_INVALIDARG;
            }

            while (pceltFetched < celt && this.position < (uint)this.threads.Length)
            {
                rgelt[pceltFetched] = this.threads[this.position];
                ++pceltFetched;
                ++this.position;
            }
            return pceltFetched == celt ? RadDbgHResult.S_OK : RadDbgHResult.S_FALSE;
        }

        public int Skip(uint celt)
        {
            uint remaining = (uint)this.threads.Length - this.position;
            uint skipped = Math.Min(celt, remaining);
            this.position += skipped;
            return skipped == celt ? RadDbgHResult.S_OK : RadDbgHResult.S_FALSE;
        }

        public int Reset()
        {
            this.position = 0;
            return RadDbgHResult.S_OK;
        }

        public int Clone(out IEnumDebugThreads2 ppEnum)
        {
            ppEnum = new RadDbgThreadEnumerator(this.threads, this.position);
            return RadDbgHResult.S_OK;
        }

        public int GetCount(out uint pcelt)
        {
            pcelt = (uint)this.threads.Length;
            return RadDbgHResult.S_OK;
        }
    }
}
