// Authored by Codex. Create ordinary workflow compiler input, without writes.
using System;
using System.IO;
using System.Reflection;
using System.Runtime.Serialization;
using System.Workflow.ComponentModel.Compiler;
using System.Xml;

public static class CreateWorkflowInput
{
    public static int Main(string[] arguments)
    {
        if (arguments.Length != 4) return 2;
        string compiler = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),
            @"Microsoft.NET\Framework64\v4.0.30319\Microsoft.Workflow.Compiler.exe");
        var parameters = new WorkflowCompilerParameters();
        parameters.GenerateInMemory = false;
        parameters.OutputAssembly = Path.GetFullPath(arguments[3]);
        parameters.CompilerOptions = "/platform:x64";
        parameters.ReferencedAssemblies.Add(Path.GetFullPath(arguments[2]));
        var assembly = Assembly.LoadFrom(compiler);
        var type = assembly.GetType("Microsoft.Workflow.Compiler.CompilerInput", true);
        object input = Activator.CreateInstance(type, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance,
            null, new object[] { parameters, new string[] { Path.GetFullPath(arguments[1]) } }, null);
        using (var writer = XmlWriter.Create(Path.GetFullPath(arguments[0])))
            new DataContractSerializer(type).WriteObject(writer, input);
        return 0;
    }
}
