using System;
using Microsoft.VisualStudio.Shell;

namespace RAD
{
    /// <summary>
    /// Registers a managed AD7 engine with Visual Studio's debugger metrics.
    /// </summary>
    [AttributeUsage(AttributeTargets.Class, AllowMultiple = false, Inherited = true)]
    internal sealed class ProvideRadDbgEngineAttribute : RegistrationAttribute
    {
        // The shell's standard provider enumerates programs on the local machine.
        private const string StandardProgramProvider = "{4FF9DEF4-8922-4D02-9379-3FFA64D1D639}";

        private readonly Type   engineType;
        private readonly string engineId;

        public ProvideRadDbgEngineAttribute(Type engineType, string engineId)
        {
            this.engineType = engineType ?? throw new ArgumentNullException(nameof(engineType));
            this.engineId   = engineId   ?? throw new ArgumentNullException(nameof(engineId));
        }

        public override void Register(RegistrationContext context)
        {
            string engineKeyPath = $"AD7Metrics\\Engine\\{{{this.engineId}}}";
            using (Key engineKey = context.CreateKey(engineKeyPath))
            {
                engineKey.SetValue(string.Empty, "RAD Debug Engine");
                engineKey.SetValue("AlwaysLoadLocal", 1);
                engineKey.SetValue("AlwaysLoadProgramProviderLocal", 1);
                engineKey.SetValue("Name", "RAD Debug Engine");
                engineKey.SetValue("CLSID", this.engineType.GUID.ToString("B"));
                engineKey.SetValue("EnginePriority", 0x50);
                engineKey.SetValue("ProgramProvider", StandardProgramProvider);
            }
        }

        public override void Unregister(RegistrationContext context)
        {
            context.RemoveKey($"AD7Metrics\\Engine\\{{{this.engineId}}}");
        }
    }
}
