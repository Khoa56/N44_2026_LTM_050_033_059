# Chạy từ PowerShell với quyền quản trị viên.
$ErrorActionPreference = 'Stop'
wsl.exe --install -d Ubuntu-22.04
if ($LASTEXITCODE -ne 0) {
    throw "Cài Ubuntu 22.04 thất bại (mã $LASTEXITCODE)."
}
Write-Host 'Sau khi cài xong, mở Ubuntu 22.04 và làm theo README của dự án.'
