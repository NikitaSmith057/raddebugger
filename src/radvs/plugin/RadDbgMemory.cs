using System;
using System.Globalization;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Debugger.Interop;

namespace RAD
{
    internal sealed class RadDbgMemoryBytes : IDebugMemoryBytes2
    {
        private readonly RadDbgEngine engine;
        private readonly ulong processHandle;

        internal RadDbgMemoryBytes(RadDbgEngine engine, ulong processHandle)
        {
            this.engine = engine;
            this.processHandle = processHandle;
        }

        public int GetSize(out ulong pqwSize)
        {
            return this.engine.GetMemorySize(this.processHandle, out pqwSize);
        }

        public int ReadAt(IDebugMemoryContext2 pStartContext, uint dwCount, byte[] rgbMemory, out uint pdwRead, ref uint pdwUnreadable)
        {
            pdwRead = 0;
            pdwUnreadable = 0;
            if (rgbMemory == null || (ulong)rgbMemory.LongLength < dwCount)
            {
                return VSConstants.E_INVALIDARG;
            }

            int result = TryGetAddress(pStartContext, out ulong address);
            if (result != VSConstants.S_OK)
            {
                return result;
            }

            result = this.engine.ReadMemoryAt(this.processHandle, address, dwCount, rgbMemory, out pdwRead, out uint unreadable);
            pdwUnreadable = unreadable;
            return result;
        }

        public int WriteAt(IDebugMemoryContext2 pStartContext, uint dwCount, byte[] rgbMemory)
        {
            if (rgbMemory == null || (ulong)rgbMemory.LongLength < dwCount)
            {
                return VSConstants.E_INVALIDARG;
            }

            int result = TryGetAddress(pStartContext, out ulong address);
            return result == VSConstants.S_OK ? this.engine.WriteMemoryAt(this.processHandle, address, dwCount, rgbMemory) : result;
        }

        private static int TryGetAddress(IDebugMemoryContext2 context, out ulong address)
        {
            address = 0;
            if (context is RadDbgCodeContext radContext)
            {
                address = radContext.Address;
                return VSConstants.S_OK;
            }
            if (context == null)
            {
                return VSConstants.E_INVALIDARG;
            }

            CONTEXT_INFO[] info = new CONTEXT_INFO[1];
            int result = context.GetInfo(enum_CONTEXT_INFO_FIELDS.CIF_ADDRESSABSOLUTE | enum_CONTEXT_INFO_FIELDS.CIF_ADDRESS, info);
            if (result < 0)
            {
                return result;
            }

            if (TryParseAddress(info[0].bstrAddressAbsolute, out address) || TryParseAddress(info[0].bstrAddress, out address))
            {
                return VSConstants.S_OK;
            }
            return VSConstants.E_INVALIDARG;
        }

        private static bool TryParseAddress(string? text, out ulong address)
        {
            address = 0;
            if (string.IsNullOrWhiteSpace(text))
            {
                return false;
            }

            string addressText = text!.Trim();
            if (addressText.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                return ulong.TryParse(addressText.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out address);
            }
            return ulong.TryParse(addressText, NumberStyles.Integer, CultureInfo.InvariantCulture, out address);
        }
    }

    internal sealed class RadDbgModule : IDebugModule2
    {
        private readonly RadDbgModuleDesc desc;

        internal RadDbgModule(RadDbgModuleDesc desc)
        {
            this.desc = desc;
        }

        public int GetInfo(enum_MODULE_INFO_FIELDS dwFields, MODULE_INFO[] pInfo)
        {
            if (pInfo == null || pInfo.Length == 0)
            {
                return VSConstants.E_INVALIDARG;
            }

            MODULE_INFO info = new MODULE_INFO();
            string moduleName = $"0x{this.desc.BaseAddress:X}";
            if ((dwFields & enum_MODULE_INFO_FIELDS.MIF_NAME) != 0)
            {
                info.m_bstrName = moduleName;
                info.dwValidFields |= enum_MODULE_INFO_FIELDS.MIF_NAME;
            }
            if ((dwFields & enum_MODULE_INFO_FIELDS.MIF_URL) != 0)
            {
                info.m_bstrUrl = moduleName;
                info.dwValidFields |= enum_MODULE_INFO_FIELDS.MIF_URL;
            }
            if ((dwFields & enum_MODULE_INFO_FIELDS.MIF_LOADADDRESS) != 0)
            {
                info.m_addrLoadAddress = this.desc.BaseAddress;
                info.dwValidFields |= enum_MODULE_INFO_FIELDS.MIF_LOADADDRESS;
            }
            if ((dwFields & enum_MODULE_INFO_FIELDS.MIF_PREFFEREDADDRESS) != 0)
            {
                info.m_addrPreferredLoadAddress = this.desc.BaseAddress;
                info.dwValidFields |= enum_MODULE_INFO_FIELDS.MIF_PREFFEREDADDRESS;
            }
            if ((dwFields & enum_MODULE_INFO_FIELDS.MIF_SIZE) != 0)
            {
                info.m_dwSize = this.desc.Size > uint.MaxValue ? uint.MaxValue : (uint)this.desc.Size;
                info.dwValidFields |= enum_MODULE_INFO_FIELDS.MIF_SIZE;
            }
            if ((dwFields & enum_MODULE_INFO_FIELDS.MIF_FLAGS) != 0)
            {
                info.m_dwModuleFlags = this.desc.SymbolState != 0 ? enum_MODULE_FLAGS.MODULE_FLAG_SYMBOLS : enum_MODULE_FLAGS.MODULE_FLAG_NONE;
                info.dwValidFields |= enum_MODULE_INFO_FIELDS.MIF_FLAGS;
            }

            pInfo[0] = info;
            return VSConstants.S_OK;
        }

        public int ReloadSymbols_Deprecated(string pszUrlToSymbols, out string pbstrDebugMessage)
        {
            pbstrDebugMessage = null!;
            return VSConstants.E_NOTIMPL;
        }
    }

    internal sealed class RadDbgProgramEnum : RadDbgEnum<IDebugProgram2, IEnumDebugPrograms2>, IEnumDebugPrograms2
    {
        internal RadDbgProgramEnum(IDebugProgram2[] data) : base(data) { }

        public int Next(uint celt, IDebugProgram2[] rgelt, ref uint pceltFetched)
        {
            return this.Move(celt, rgelt, ref pceltFetched);
        }
    }

    internal sealed class RadDbgModuleEnum : RadDbgEnum<IDebugModule2, IEnumDebugModules2>, IEnumDebugModules2
    {
        internal RadDbgModuleEnum(IDebugModule2[] data) : base(data) { }

        public int Next(uint celt, IDebugModule2[] rgelt, ref uint pceltFetched)
        {
            return this.Move(celt, rgelt, ref pceltFetched);
        }
    }
}
