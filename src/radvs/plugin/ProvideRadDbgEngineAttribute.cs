using System;
using Microsoft.VisualStudio.Shell;

namespace RAD
{
    [AttributeUsage(AttributeTargets.Class, AllowMultiple = false, Inherited = true)]
    internal sealed class ProvideRadDbgEngineAttribute : RegistrationAttribute
    {
        private const string StandardProgramProvider = "{4FF9DEF4-8922-4D02-9379-3FFA64D1D639}";

        private readonly Type engineType;
        private readonly string engineId;

        public ProvideRadDbgEngineAttribute(Type engineType, string engineId)
        {
            this.engineType = engineType ?? throw new ArgumentNullException(nameof(engineType));
            this.engineId = engineId ?? throw new ArgumentNullException(nameof(engineId));
        }

        public override void Register(RegistrationContext context)
        {
            using (Key engineKey = context.CreateKey($"AD7Metrics\\Engine\\{{{this.engineId}}}"))
            {
                engineKey.SetValue(string.Empty, "RAD Debug Engine");
                engineKey.SetValue("AlwaysLoadLocal", 1);
                engineKey.SetValue("AlwaysLoadProgramProviderLocal", 1);
                engineKey.SetValue("EnginePriority", 0x50);
                engineKey.SetValue("Name", "RAD Debug Engine");
                engineKey.SetValue("CLSID", this.engineType.GUID.ToString("B"));
                engineKey.SetValue("ProgramProvider", StandardProgramProvider);
            }
        }

        public override void Unregister(RegistrationContext context)
        {
            context.RemoveKey($"AD7Metrics\\Engine\\{{{this.engineId}}}");
        }
    }
}
