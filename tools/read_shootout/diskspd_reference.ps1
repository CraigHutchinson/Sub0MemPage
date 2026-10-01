# diskspd_reference.ps1 -- the DiskSpd reference runs the shootout is compared against (README.md, "DiskSpd
# reference"). DiskSpd (https://github.com/microsoft/diskspd) is Microsoft's storage load generator. It shows
# what this host's drive and I/O stack deliver, so a shootout arm far below it is losing time in its own code.
#
#   pwsh tools/read_shootout/diskspd_reference.ps1 -DiskSpd D:\tools\DiskSpd\amd64\diskspd.exe -File <file>
#
# Read-only (-w0). -Sh = non-cached (FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH); no -S = cached.
param(
    [Parameter(Mandatory)] [string] $DiskSpd,
    [Parameter(Mandatory)] [string] $File,
    [int] $Seconds = 10
)
$runs = [ordered]@{
    "uncached 2 MiB, depth 1"                      = @("-b2M", "-o1", "-t1", "-Sh", "-r")
    "uncached 2 MiB, depth 16"                     = @("-b2M", "-o16", "-t1", "-Sh", "-r")
    "uncached 256 KiB, depth 1"                    = @("-b256K", "-o1", "-t1", "-Sh", "-r")
    "uncached 256 KiB, depth 16"                   = @("-b256K", "-o16", "-t1", "-Sh", "-r")
    "uncached 256 KiB, 8 threads x 1 (pool shape)" = @("-b256K", "-o1", "-t8", "-Sh", "-r")
    "uncached 256 KiB, 8 threads, adjacent blocks" = @("-b256K", "-o1", "-t8", "-Sh", "-si")
    "uncached 256 KiB, bursts of 7 + think time"   = @("-b256K", "-o7", "-t1", "-Sh", "-r", "-i7", "-j3")
    "cached 2 MiB, depth 16"                       = @("-b2M", "-o16", "-t1", "-r")
}
"{0,-46} {1,9} {2,9} {3,9} {4,9}" -f "run", "MiB/s", "IOPS", "p50 ms", "p90 ms"
foreach ($name in $runs.Keys) {
    $out = & $DiskSpd @($runs[$name]) -w0 "-d$Seconds" -W2 -L $File 2>&1
    $total = ($out | Select-String "^total:" | Select-Object -First 1).Line -split '\|'
    $p50 = (($out | Select-String "^\s+50th" | Select-Object -First 1).Line -split '\|')[1]
    $p90 = (($out | Select-String "^\s+90th" | Select-Object -First 1).Line -split '\|')[1]
    "{0,-46} {1,9} {2,9} {3,9} {4,9}" -f $name, $total[2].Trim(), $total[3].Trim(), $p50.Trim(), $p90.Trim()
}
