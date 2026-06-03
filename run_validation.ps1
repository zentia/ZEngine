$env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
Set-Location 'e:\Engine\ZEngine\bin\RelWithDebInfo'
$p = Start-Process -FilePath '.\ZEditor.exe' -ArgumentList @('-p','E:\ZEngineDemo\ZEngineDemo.zproject','--rhi','vulkan') -PassThru -Wait
Write-Host ('Editor exit: ' + $p.ExitCode)
