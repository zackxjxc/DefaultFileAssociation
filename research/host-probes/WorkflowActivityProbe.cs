// Authored by Codex. A local workflow validation callback; no UI or resources.
using System.Workflow.ComponentModel;

public sealed class PdfAssociationWorkflowProbe : Activity
{
    public PdfAssociationWorkflowProbe()
    {
        ProbeHostEntry.Run("Microsoft.Workflow.Compiler.exe");
    }
}
