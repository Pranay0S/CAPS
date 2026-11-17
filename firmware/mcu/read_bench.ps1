# Prints the benchmark report from the Nucleo's ST-Link virtual COM port.
#   powershell -File firmware/mcu/read_bench.ps1            # auto-detects the ST-Link port
#   powershell -File firmware/mcu/read_bench.ps1 -Port COM5
param([string]$Port = "", [int]$Seconds = 6)

if (-not $Port) {
    $dev = Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'STLink.*\((COM\d+)\)' } | Select-Object -First 1
    if (-not $dev) {
        Write-Error "No ST-Link virtual COM port found. Is the Nucleo plugged in? Ports: $([System.IO.Ports.SerialPort]::GetPortNames() -join ', ')"
        exit 1
    }
    $Port = [regex]::Match($dev.Name, 'COM\d+').Value
}

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.ReadTimeout = 500
$sp.Open()
try {
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) {
        try { Write-Host -NoNewline $sp.ReadExisting() } catch {}
        Start-Sleep -Milliseconds 100
    }
} finally {
    $sp.Close()
}
