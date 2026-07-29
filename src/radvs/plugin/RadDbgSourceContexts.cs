using System;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    internal sealed class RadDbgFrameInfoEnum : IEnumDebugFrameInfo2
    {
        private readonly FRAMEINFO[] frames;
        private uint position;

        internal RadDbgFrameInfoEnum(FRAMEINFO[] frames)
        {
            this.frames = frames;
        }

        public int Clone(out IEnumDebugFrameInfo2 ppEnum)
        {
            ppEnum = null!;
            return VSConstants.E_NOTIMPL;
        }

        public int GetCount(out uint pcelt)
        {
            pcelt = (uint)this.frames.Length;
            return VSConstants.S_OK;
        }

        public int Next(uint celt, FRAMEINFO[] rgelt, ref uint pceltFetched)
        {
            uint remaining = (uint)this.frames.Length - this.position;
            pceltFetched = Math.Min(celt, remaining);
            for (uint index = 0; index < pceltFetched; index++)
            {
                rgelt[index] = this.frames[this.position + index];
            }
            this.position += pceltFetched;
            return pceltFetched == celt ? VSConstants.S_OK : VSConstants.S_FALSE;
        }

        public int Reset()
        {
            this.position = 0;
            return VSConstants.S_OK;
        }

        public int Skip(uint celt)
        {
            uint remaining = (uint)this.frames.Length - this.position;
            uint skipped = Math.Min(celt, remaining);
            this.position += skipped;
            return skipped == celt ? VSConstants.S_OK : VSConstants.S_FALSE;
        }
    }

    internal sealed class RadDbgCodeContextEnum : IEnumDebugCodeContexts2
    {
        private readonly IDebugCodeContext2[] contexts;
        private uint position;

        internal RadDbgCodeContextEnum(IDebugCodeContext2[] contexts)
        {
            this.contexts = contexts;
        }

        public int Clone(out IEnumDebugCodeContexts2 ppEnum)
        {
            ppEnum = null!;
            return VSConstants.E_NOTIMPL;
        }

        public int GetCount(out uint pcelt)
        {
            pcelt = (uint)this.contexts.Length;
            return VSConstants.S_OK;
        }

        public int Next(uint celt, IDebugCodeContext2[] rgelt, ref uint pceltFetched)
        {
            uint remaining = (uint)this.contexts.Length - this.position;
            pceltFetched = Math.Min(celt, remaining);
            for (uint index = 0; index < pceltFetched; index++)
            {
                rgelt[index] = this.contexts[this.position + index];
            }
            this.position += pceltFetched;
            return pceltFetched == celt ? VSConstants.S_OK : VSConstants.S_FALSE;
        }

        public int Reset()
        {
            this.position = 0;
            return VSConstants.S_OK;
        }

        public int Skip(uint celt)
        {
            uint remaining = (uint)this.contexts.Length - this.position;
            uint skipped = Math.Min(celt, remaining);
            this.position += skipped;
            return skipped == celt ? VSConstants.S_OK : VSConstants.S_FALSE;
        }
    }

    internal sealed class RadDbgThreadEnum : IEnumDebugThreads2
    {
        private readonly IDebugThread2[] threads;
        private uint position;

        internal RadDbgThreadEnum(IDebugThread2[] threads)
        {
            this.threads = threads;
        }

        public int Clone(out IEnumDebugThreads2 ppEnum)
        {
            ppEnum = null!;
            return VSConstants.E_NOTIMPL;
        }

        public int GetCount(out uint pcelt)
        {
            pcelt = (uint)this.threads.Length;
            return VSConstants.S_OK;
        }

        public int Next(uint celt, IDebugThread2[] rgelt, ref uint pceltFetched)
        {
            uint remaining = (uint)this.threads.Length - this.position;
            pceltFetched = Math.Min(celt, remaining);
            for (uint index = 0; index < pceltFetched; index++)
            {
                rgelt[index] = this.threads[this.position + index];
            }
            this.position += pceltFetched;
            return pceltFetched == celt ? VSConstants.S_OK : VSConstants.S_FALSE;
        }

        public int Reset()
        {
            this.position = 0;
            return VSConstants.S_OK;
        }

        public int Skip(uint celt)
        {
            uint remaining = (uint)this.threads.Length - this.position;
            uint skipped = Math.Min(celt, remaining);
            this.position += skipped;
            return skipped == celt ? VSConstants.S_OK : VSConstants.S_FALSE;
        }
    }

    internal sealed class RadDbgStackFrame : IDebugStackFrame2
    {
        private readonly RadDbgThread thread;
        private readonly RadDbgFrameInfo frame;
        private readonly RadDbgCodeContext codeContext;
        private readonly RadDbgDocumentContext? documentContext;

        internal RadDbgStackFrame(RadDbgThread thread, RadDbgFrameInfo frame, string sourcePath)
        {
            this.thread = thread;
            this.frame = frame;
            this.codeContext = new RadDbgCodeContext(frame.InstructionPointer);
            if (frame.HasSource != 0 && !string.IsNullOrEmpty(sourcePath))
            {
                this.documentContext = new RadDbgDocumentContext(sourcePath, frame.SourceLine, frame.SourceColumn, this.codeContext);
                this.codeContext.SetDocumentContext(this.documentContext);
            }
        }

        internal void GetFrameInfo(enum_FRAMEINFO_FLAGS fields, out FRAMEINFO frameInfo)
        {
            frameInfo = new FRAMEINFO();
            if ((fields & enum_FRAMEINFO_FLAGS.FIF_FUNCNAME) != 0)
            {
                frameInfo.m_bstrFuncName = $"0x{this.frame.InstructionPointer:X}";
                frameInfo.m_dwValidFields |= enum_FRAMEINFO_FLAGS.FIF_FUNCNAME;
            }
            if ((fields & enum_FRAMEINFO_FLAGS.FIF_FRAME) != 0)
            {
                frameInfo.m_pFrame = this;
                frameInfo.m_dwValidFields |= enum_FRAMEINFO_FLAGS.FIF_FRAME;
            }
            if ((fields & enum_FRAMEINFO_FLAGS.FIF_DEBUGINFO) != 0)
            {
                frameInfo.m_fHasDebugInfo = this.documentContext != null ? 1 : 0;
                frameInfo.m_dwValidFields |= enum_FRAMEINFO_FLAGS.FIF_DEBUGINFO;
            }
            if ((fields & enum_FRAMEINFO_FLAGS.FIF_FLAGS) != 0)
            {
                if (this.documentContext == null)
                {
                    frameInfo.m_dwFlags |= (uint)enum_FRAMEINFO_FLAGS_VALUES.FIFV_ANNOTATEDFRAME;
                }
                frameInfo.m_dwValidFields |= enum_FRAMEINFO_FLAGS.FIF_FLAGS;
            }
        }

        public int EnumProperties(enum_DEBUGPROP_INFO_FLAGS dwFields, uint nRadix, ref Guid guidFilter, uint dwTimeout, out uint pcelt, out IEnumDebugPropertyInfo2 ppEnum)
        {
            pcelt = 0;
            ppEnum = null!;
            return VSConstants.E_NOTIMPL;
        }

        public int GetCodeContext(out IDebugCodeContext2 ppCodeCxt)
        {
            ppCodeCxt = this.codeContext;
            return VSConstants.S_OK;
        }

        public int GetDebugProperty(out IDebugProperty2 ppProperty)
        {
            ppProperty = null!;
            return VSConstants.E_NOTIMPL;
        }

        public int GetDocumentContext(out IDebugDocumentContext2 ppCxt)
        {
            ppCxt = this.documentContext!;
            return this.documentContext != null ? VSConstants.S_OK : VSConstants.E_FAIL;
        }

        public int GetExpressionContext(out IDebugExpressionContext2 ppExprCxt)
        {
            ppExprCxt = null!;
            return VSConstants.E_NOTIMPL;
        }

        public int GetInfo(enum_FRAMEINFO_FLAGS dwFieldSpec, uint nRadix, FRAMEINFO[] pFrameInfo)
        {
            this.GetFrameInfo(dwFieldSpec, out pFrameInfo[0]);
            return VSConstants.S_OK;
        }

        public int GetLanguageInfo(ref string pbstrLanguage, ref Guid pguidLanguage)
        {
            return this.documentContext != null ? this.documentContext.GetLanguageInfo(ref pbstrLanguage, ref pguidLanguage) : VSConstants.S_FALSE;
        }

        public int GetName(out string pbstrName)
        {
            pbstrName = $"0x{this.frame.InstructionPointer:X}";
            return VSConstants.S_OK;
        }

        public int GetPhysicalStackRange(out ulong paddrMin, out ulong paddrMax)
        {
            paddrMin = 0;
            paddrMax = 0;
            return VSConstants.S_OK;
        }

        public int GetThread(out IDebugThread2 ppThread)
        {
            ppThread = this.thread;
            return VSConstants.S_OK;
        }
    }

    internal sealed class RadDbgCodeContext : IDebugCodeContext2
    {
        private readonly ulong address;
        private IDebugDocumentContext2? documentContext;

        internal RadDbgCodeContext(ulong address)
        {
            this.address = address;
        }

        internal ulong Address => this.address;

        internal void SetDocumentContext(IDebugDocumentContext2 documentContext)
        {
            this.documentContext = documentContext;
        }

        public int Add(ulong dwCount, out IDebugMemoryContext2 ppMemCxt)
        {
            ppMemCxt = new RadDbgCodeContext(this.address + dwCount);
            return VSConstants.S_OK;
        }

        public int Compare(enum_CONTEXT_COMPARE dwCompare, IDebugMemoryContext2[] rgpMemoryContextSet, uint dwMemoryContextSetLen, out uint pdwMemoryContext)
        {
            pdwMemoryContext = uint.MaxValue;
            if (dwCompare != enum_CONTEXT_COMPARE.CONTEXT_EQUAL)
            {
                return VSConstants.E_NOTIMPL;
            }
            for (uint index = 0; index < dwMemoryContextSetLen; index++)
            {
                if (rgpMemoryContextSet[index] is RadDbgCodeContext context && context.address == this.address)
                {
                    pdwMemoryContext = index;
                    return VSConstants.S_OK;
                }
            }
            return VSConstants.S_FALSE;
        }

        public int GetInfo(enum_CONTEXT_INFO_FIELDS dwFields, CONTEXT_INFO[] pInfo)
        {
            pInfo[0].dwFields = 0;
            string addressText = $"0x{this.address:X}";
            if ((dwFields & enum_CONTEXT_INFO_FIELDS.CIF_ADDRESS) != 0)
            {
                pInfo[0].bstrAddress = addressText;
                pInfo[0].dwFields |= enum_CONTEXT_INFO_FIELDS.CIF_ADDRESS;
            }
            if ((dwFields & enum_CONTEXT_INFO_FIELDS.CIF_ADDRESSABSOLUTE) != 0)
            {
                pInfo[0].bstrAddressAbsolute = addressText;
                pInfo[0].dwFields |= enum_CONTEXT_INFO_FIELDS.CIF_ADDRESSABSOLUTE;
            }
            return VSConstants.S_OK;
        }

        public int GetName(out string pbstrName)
        {
            pbstrName = $"0x{this.address:X}";
            return VSConstants.S_OK;
        }

        public int Subtract(ulong dwCount, out IDebugMemoryContext2 ppMemCxt)
        {
            ppMemCxt = new RadDbgCodeContext(this.address - dwCount);
            return VSConstants.S_OK;
        }

        public int GetDocumentContext(out IDebugDocumentContext2 ppSrcCxt)
        {
            ppSrcCxt = this.documentContext!;
            return this.documentContext != null ? VSConstants.S_OK : VSConstants.S_FALSE;
        }

        public int GetLanguageInfo(ref string pbstrLanguage, ref Guid pguidLanguage)
        {
            return this.documentContext != null ? this.documentContext.GetLanguageInfo(ref pbstrLanguage, ref pguidLanguage) : VSConstants.S_FALSE;
        }
    }

    internal sealed class RadDbgDocumentContext : IDebugDocumentContext2
    {
        private readonly string path;
        private readonly TEXT_POSITION position;
        private readonly IDebugCodeContext2 codeContext;

        internal RadDbgDocumentContext(string path, uint oneBasedLine, uint oneBasedColumn, IDebugCodeContext2 codeContext)
        {
            this.path = path;
            this.position = new TEXT_POSITION
            {
                dwLine = oneBasedLine > 0 ? oneBasedLine - 1 : 0,
                dwColumn = oneBasedColumn > 0 ? oneBasedColumn - 1 : 0,
            };
            this.codeContext = codeContext;
        }

        public int Compare(enum_DOCCONTEXT_COMPARE dwCompare, IDebugDocumentContext2[] rgpDocContextSet, uint dwDocContextSetLen, out uint pdwDocContext)
        {
            pdwDocContext = uint.MaxValue;
            return VSConstants.E_NOTIMPL;
        }

        public int EnumCodeContexts(out IEnumDebugCodeContexts2 ppEnumCodeCxts)
        {
            ppEnumCodeCxts = new RadDbgCodeContextEnum([this.codeContext]);
            return VSConstants.S_OK;
        }

        public int GetDocument(out IDebugDocument2 ppDocument)
        {
            ppDocument = null!;
            return VSConstants.E_FAIL;
        }

        public int GetLanguageInfo(ref string pbstrLanguage, ref Guid pguidLanguage)
        {
            pbstrLanguage = "C++";
            pguidLanguage = Guid.Empty;
            return VSConstants.S_OK;
        }

        public int GetName(enum_GETNAME_TYPE gnType, out string pbstrFileName)
        {
            pbstrFileName = this.path;
            return VSConstants.S_OK;
        }

        public int GetSourceRange(TEXT_POSITION[] pBegPosition, TEXT_POSITION[] pEndPosition)
        {
            pBegPosition[0] = this.position;
            pEndPosition[0] = this.position;
            return VSConstants.S_OK;
        }

        public int GetStatementRange(TEXT_POSITION[] pBegPosition, TEXT_POSITION[] pEndPosition)
        {
            pBegPosition[0] = this.position;
            pEndPosition[0] = this.position;
            return VSConstants.S_OK;
        }

        public int Seek(int nCount, out IDebugDocumentContext2 ppDocContext)
        {
            ppDocContext = null!;
            return VSConstants.E_NOTIMPL;
        }
    }
}
