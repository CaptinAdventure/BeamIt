<#
.SYNOPSIS
    Starts a BeamIt WSS host instance, creating its password file and TLS certificate if needed.

.DESCRIPTION
    Each instance keeps its files in a separate directory (default %LOCALAPPDATA%\BeamIt\<Instance>):
      password.txt  random password (32 bytes, base64url) unless one already exists
      cert.pem      self-signed RSA-2048 server certificate (2 years; renewed when within 30 days of expiry)
      key.pem       unencrypted PKCS#8 private key for cert.pem
    The directory, password and key are restricted to the current user (inheritance removed). The password is never
    printed; read it from the file to give it to clients. Clients should pin the printed certificate fingerprint.

.EXAMPLE
    .\scripts\Start-BeamItServer.ps1
    Before starting, the script checks that the client microphone has somewhere to go. With -MicrophoneOutput auto
    (default) that is the BeamIt Microphone driver (\\.\BeamItMicCtl), else VB-Audio Virtual Cable's playback
    device ("CABLE Input"); with beamit only the driver; with vb-cable only the cable; otherwise the named playback
    device. Without one, host audio
    still plays, but client microphone audio is discarded. With -MicrophoneDriver, a missing driver is installed from
    that signed package (UAC prompt, BeamItMicProducer.exe --install). -RequireMicrophone refuses to start the host
    while no microphone output is available.

.EXAMPLE
    .\scripts\Start-BeamItServer.ps1 -Instance teams -Port 17390 -Bind 192.168.1.10
.EXAMPLE
    .\scripts\Start-BeamItServer.ps1 -MicrophoneDriver C:\Package\BeamItMicrophone.inf -RequireMicrophone
.EXAMPLE
    .\scripts\Start-BeamItServer.ps1 -MicrophoneOutput vb-cable -RequireMicrophone
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9_.-]+$')]
    [string] $Instance = 'default',
    [ValidateRange(1, 65535)]
    [int] $Port = 17389,
    [string] $Bind = '0.0.0.0',
    # Path to BeamIt.exe; defaults to BeamIt.exe next to this script, else the newest cmake-build-* build.
    [string] $Executable,
    # Instance directory; defaults to %LOCALAPPDATA%\BeamIt\<Instance>.
    [string] $DataDirectory,
    # Where client microphone audio goes: auto (BeamIt Microphone driver, else VB-Audio Virtual Cable),
    # beamit (driver only), vb-cable or the name of a playback device (passed to BeamIt as --microphone-output).
    [string] $MicrophoneOutput = 'auto',
    # Signed BeamItMicrophone.inf to install (elevated) when no microphone output is available (auto/beamit only).
    [string] $MicrophoneDriver,
    # Do not start the host unless a microphone output is available.
    [switch] $RequireMicrophone,
    # Extra BeamIt arguments, passed through unchanged.
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $BeamItArguments = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Set-OwnerOnlyAcl([string] $Path, [switch] $Directory) {
    $acl = if ($Directory) { New-Object System.Security.AccessControl.DirectorySecurity }
           else { New-Object System.Security.AccessControl.FileSecurity }
    $acl.SetAccessRuleProtection($true, $false)
    $user = [System.Security.Principal.WindowsIdentity]::GetCurrent().User
    $inherit = if ($Directory) { 'ContainerInherit, ObjectInherit' } else { 'None' }
    $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
        $user, 'FullControl', $inherit, 'None', 'Allow')))
    # .NET directly (not Set-Acl), so no account-name lookup through the domain is needed.
    $info = if ($Directory) { New-Object System.IO.DirectoryInfo($Path) } else { New-Object System.IO.FileInfo($Path) }
    if ($info.PSObject.Methods['SetAccessControl']) { $info.SetAccessControl($acl) } # Windows PowerShell 5.1
    else { [System.IO.FileSystemAclExtensions]::SetAccessControl($info, $acl) }       # PowerShell 7
}

function Write-SecretFile([string] $Path, [string] $Content) {
    # Create empty and restrict before writing, so the secret is never readable by others.
    [System.IO.File]::WriteAllText($Path, '')
    Set-OwnerOnlyAcl $Path
    [System.IO.File]::WriteAllText($Path, $Content, (New-Object System.Text.UTF8Encoding($false)))
}

function ConvertTo-Pem([byte[]] $Der, [string] $Label) {
    $base64 = [Convert]::ToBase64String($Der)
    $lines = for ($i = 0; $i -lt $base64.Length; $i += 64) { $base64.Substring($i, [Math]::Min(64, $base64.Length - $i)) }
    "-----BEGIN $Label-----`n" + ($lines -join "`n") + "`n-----END $Label-----`n"
}

function Get-PemCertificate([string] $Path) {
    $text = [System.IO.File]::ReadAllText($Path)
    $match = [regex]::Match($text, '-----BEGIN CERTIFICATE-----([A-Za-z0-9+/=\s]+)-----END CERTIFICATE-----')
    if (-not $match.Success) { return $null }
    $bytes = [Convert]::FromBase64String(($match.Groups[1].Value -replace '\s', ''))
    New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(, $bytes)
}

function New-ServerCertificate([string] $CertPath, [string] $KeyPath) {
    $crypto = 'System.Security.Cryptography'
    $x509 = "$crypto.X509Certificates"
    # Ephemeral CNG keys allow plaintext PKCS#8 export without touching a key store.
    $rsa = New-Object "$crypto.RSACng"(2048)
    try {
        $hostName = [System.Net.Dns]::GetHostName()
        $request = New-Object "$x509.CertificateRequest"("CN=BeamIt host ($Instance)", $rsa,
            [System.Security.Cryptography.HashAlgorithmName]::SHA256,
            [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
        $san = New-Object "$x509.SubjectAlternativeNameBuilder"
        $san.AddDnsName($hostName)
        $san.AddDnsName('localhost')
        $san.AddIpAddress([System.Net.IPAddress]::Loopback)
        foreach ($address in [System.Net.Dns]::GetHostAddresses($hostName)) {
            if ($address.AddressFamily -eq 'InterNetwork' -or -not $address.IsIPv6LinkLocal) { $san.AddIpAddress($address) }
        }
        $request.CertificateExtensions.Add($san.Build())
        $request.CertificateExtensions.Add((New-Object "$x509.X509BasicConstraintsExtension"($false, $false, 0, $true)))
        $request.CertificateExtensions.Add((New-Object "$x509.X509KeyUsageExtension"(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]'DigitalSignature, KeyEncipherment', $true)))
        $serverAuth = New-Object "$crypto.OidCollection"
        [void]$serverAuth.Add((New-Object "$crypto.Oid"('1.3.6.1.5.5.7.3.1')))
        $request.CertificateExtensions.Add((New-Object "$x509.X509EnhancedKeyUsageExtension"($serverAuth, $false)))

        $now = [DateTimeOffset]::UtcNow
        $certificate = $request.CreateSelfSigned($now.AddMinutes(-5), $now.AddYears(2))
        $pkcs8 = $rsa.Key.Export([System.Security.Cryptography.CngKeyBlobFormat]::Pkcs8PrivateBlob)
        try {
            Write-SecretFile $KeyPath (ConvertTo-Pem $pkcs8 'PRIVATE KEY')
        } finally {
            [Array]::Clear($pkcs8, 0, $pkcs8.Length)
        }
        [System.IO.File]::WriteAllText($CertPath, (ConvertTo-Pem $certificate.RawData 'CERTIFICATE'))
        $certificate.Dispose()
    } finally {
        $rsa.Dispose()
    }
}

# Opens the driver's control device exactly like the host does (opening does not take the producer lease).
# Returns $null when available, otherwise a reason.
function Test-BeamItMicrophone {
    if (-not ('BeamIt.MicrophoneProbe' -as [type])) {
        Add-Type -Namespace BeamIt -Name MicrophoneProbe -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern Microsoft.Win32.SafeHandles.SafeFileHandle CreateFileW(string name, uint access, uint share,
    System.IntPtr security, uint disposition, uint flags, System.IntPtr template);
public static int Open() {
    using (var handle = CreateFileW(@"\\.\BeamItMicCtl", 0xC0000000u, 3u, System.IntPtr.Zero, 3u, 0x80u, System.IntPtr.Zero)) {
        return handle.IsInvalid ? Marshal.GetLastWin32Error() : 0;
    }
}
'@
    }
    $errorCode = [BeamIt.MicrophoneProbe]::Open()
    if ($errorCode -eq 0) { return $null }
    if ($errorCode -in 2, 3) {
        $device = Get-PnpDevice -ErrorAction SilentlyContinue |
            Where-Object { $_.HardwareID -contains 'Root\BeamItMicrophone' } | Select-Object -First 1
        if ($device) { return "the BeamIt Microphone device exists but is not working (status $($device.Status), problem $($device.Problem)); a reboot may be required" }
        return 'the BeamIt Microphone driver is not installed'
    }
    "\\.\BeamItMicCtl cannot be opened ($((New-Object System.ComponentModel.Win32Exception($errorCode)).Message))"
}

function Install-BeamItMicrophone([string] $Inf) {
    if (-not (Test-Path -LiteralPath $Inf -PathType Leaf)) { throw "Microphone driver package not found: $Inf" }
    $Inf = (Resolve-Path -LiteralPath $Inf).Path
    $packageDirectory = Split-Path -Parent $Inf
    # Build layout: bin\x64\Release\BeamItMicProducer.exe next to the BeamItMicDriver\ package directory.
    $producer = @((Join-Path $packageDirectory 'BeamItMicProducer.exe'),
                  (Join-Path (Split-Path -Parent $packageDirectory) 'BeamItMicProducer.exe'),
                  (Join-Path $PSScriptRoot 'BeamItMicProducer.exe'),
                  (Join-Path $repository 'drivers\BeamItMicrophone\bin\x64\Release\BeamItMicProducer.exe')) |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $producer) { throw 'BeamItMicProducer.exe not found next to the driver package, this script or the driver build output.' }
    $quote = { param($text) "'" + ($text -replace "'", "''") + "'" }
    $command = "& $(& $quote $producer) --install $(& $quote $Inf); " +
               "if (`$LASTEXITCODE) { Read-Host 'Installation failed. Press Enter to close' }; exit `$LASTEXITCODE"
    Write-Host "Installing the BeamIt Microphone driver from $Inf (administrator approval required)..."
    $process = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $command)
    if ($process.ExitCode -ne 0) { throw "BeamIt Microphone driver installation failed (exit code $($process.ExitCode))." }
}

# Active playback endpoint names as WASAPI reports them ("<endpoint name> (<device description>)"). PnP
# AudioEndpoint names can differ, so read the MMDevices registry (readable without elevation).
function Get-PlaybackDeviceNames {
    $root = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
    @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue | ForEach-Object {
        if ((Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue).DeviceState -ne 1) { return }
        $properties = Get-ItemProperty -LiteralPath (Join-Path $_.PSPath 'Properties') -ErrorAction SilentlyContinue
        if (-not $properties) { return }
        $name = $properties.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
        $description = $properties.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
        if ($name) { if ($description) { "$name ($description)" } else { $name } }
    })
}

# Matches playback endpoints like the host: an exact friendly name, else a unique substring. 'vb-cable' looks for
# "CABLE Input", else the single VB-Audio Virtual Cable endpoint other than "CABLE In 16ch".
# Returns $null when exactly one matches, otherwise a reason.
function Test-PlaybackDevice([string] $Name) {
    $names = Get-PlaybackDeviceNames
    if ($Name -eq 'vb-cable') {
        if (-not (Test-PlaybackDevice 'CABLE Input')) { return $null }
        $cables = @($names | Where-Object { $_ -like '*(VB-Audio Virtual Cable)*' -and $_ -notmatch '16 ?ch' })
        if ($cables.Count -eq 1) { return $null }
        if ($cables.Count -eq 0) { return 'VB-Audio Virtual Cable is not installed' }
        return "several VB-Audio Virtual Cable playback devices exist ($($cables -join ', ')); pass -MicrophoneOutput with one name"
    }
    if (@($names | Where-Object { $_ -eq $Name }).Count -eq 1) { return $null }
    $found = @($names | Where-Object { $_.IndexOf($Name, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
    if ($found.Count -eq 1) { return $null }
    $available = if ($names) { $names -join ', ' } else { 'none' }
    if ($found.Count -eq 0) { return "no playback device matches '$Name' (active playback devices: $available)" }
    "several playback devices match '$Name' ($($found -join ', ')); use a longer name"
}

$repository = Split-Path -Parent $PSScriptRoot
$usesDriver = $MicrophoneOutput -in 'auto', 'beamit'
$microphoneProblem = if ($usesDriver) { Test-BeamItMicrophone } else { Test-PlaybackDevice $MicrophoneOutput }
$cableTarget = "VB-Audio Virtual Cable (select 'CABLE Output' as the microphone in Teams)"
$microphoneTarget = if ($usesDriver) { 'BeamIt Microphone driver' }
                    elseif ($MicrophoneOutput -eq 'vb-cable') { $cableTarget }
                    else { "playback device '$MicrophoneOutput'" }
if ($microphoneProblem -and $MicrophoneOutput -eq 'auto') {
    $cableProblem = Test-PlaybackDevice 'vb-cable'
    if (-not $cableProblem) {
        $microphoneProblem = $null
        $microphoneTarget = $cableTarget
    } else {
        $microphoneProblem = "$microphoneProblem, and $($cableProblem.Substring(0, 1).ToLower())$($cableProblem.Substring(1))"
    }
}
if ($microphoneProblem -and $MicrophoneDriver -and $usesDriver) {
    Write-Warning "Microphone output unavailable: $microphoneProblem."
    Install-BeamItMicrophone $MicrophoneDriver
    $microphoneProblem = Test-BeamItMicrophone
    $microphoneTarget = 'BeamIt Microphone driver'
}
if (-not $microphoneProblem) {
    Write-Host "Client microphone audio will be delivered to the $microphoneTarget."
} elseif ($RequireMicrophone) {
    throw "Microphone output unavailable: $microphoneProblem. Install VB-Audio Virtual Cable or the signed BeamIt Microphone driver (see drivers\BeamItMicrophone\README.md), pass -MicrophoneOutput <playback device> or -MicrophoneDriver <BeamItMicrophone.inf>, or omit -RequireMicrophone."
} else {
    Write-Warning "Microphone output unavailable: $microphoneProblem. Host audio will play, but client microphone audio is discarded. Install VB-Audio Virtual Cable, pass -MicrophoneDriver <BeamItMicrophone.inf> to install the BeamIt driver, or -RequireMicrophone to refuse to start."
}

if (-not $Executable) {
    # Release package: BeamIt.exe sits next to this script. Repository: newest build in any configuration.
    $local = Join-Path $PSScriptRoot 'BeamIt.exe'
    $candidate = if (Test-Path -LiteralPath $local) { Get-Item -LiteralPath $local } else {
        Get-ChildItem -Path $repository -Filter 'cmake-build-*' -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { foreach ($sub in '', 'Release', 'RelWithDebInfo', 'MinSizeRel', 'Debug') {
                Join-Path $_.FullName (Join-Path $sub 'BeamIt.exe') } } |
            Where-Object { Test-Path -LiteralPath $_ } |
            Get-Item | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    }
    if (-not $candidate) { throw "BeamIt.exe not found next to this script or under $repository\cmake-build-*; build it or pass -Executable." }
    $Executable = $candidate.FullName
} elseif (-not (Test-Path -LiteralPath $Executable)) {
    throw "Executable not found: $Executable"
}

if (-not $DataDirectory) { $DataDirectory = Join-Path $env:LOCALAPPDATA "BeamIt\$Instance" }
if (-not (Test-Path -LiteralPath $DataDirectory)) {
    New-Item -ItemType Directory -Path $DataDirectory | Out-Null
    Set-OwnerOnlyAcl $DataDirectory -Directory
}
$passwordFile = Join-Path $DataDirectory 'password.txt'
$certFile = Join-Path $DataDirectory 'cert.pem'
$keyFile = Join-Path $DataDirectory 'key.pem'

if (-not (Test-Path -LiteralPath $passwordFile) -or
    [string]::IsNullOrWhiteSpace([System.IO.File]::ReadAllText($passwordFile))) {
    $bytes = New-Object byte[] 32
    $generator = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $generator.GetBytes($bytes)
    $generator.Dispose()
    Write-SecretFile $passwordFile ([Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_'))
    [Array]::Clear($bytes, 0, $bytes.Length)
    Write-Host "Created password file $passwordFile (give its contents to clients)."
} else {
    Write-Host "Using password file $passwordFile"
}

$certificate = $null
if ((Test-Path -LiteralPath $certFile) -and (Test-Path -LiteralPath $keyFile)) { $certificate = Get-PemCertificate $certFile }
if (-not $certificate -or $certificate.NotAfter -lt (Get-Date).AddDays(30)) {
    if ($certificate) { Write-Host 'Certificate expires within 30 days; generating a new one.' }
    New-ServerCertificate $certFile $keyFile
    $certificate = Get-PemCertificate $certFile
    Write-Host "Created self-signed certificate $certFile (key $keyFile)."
} else {
    Write-Host "Using certificate $certFile"
}
$sha256 = [System.Security.Cryptography.SHA256]::Create()
$fingerprint = ($sha256.ComputeHash($certificate.RawData) | ForEach-Object { $_.ToString('X2') }) -join ':'
$sha256.Dispose()
Write-Host "Certificate SHA-256 fingerprint: $fingerprint (valid until $($certificate.NotAfter))"
$certificate.Dispose()

$arguments = @('--port', $Port, '--bind', $Bind,
               '--password-file', $passwordFile, '--tls-cert', $certFile, '--tls-key', $keyFile,
               '--microphone-output', $MicrophoneOutput) + $BeamItArguments
$addresses = if ($Bind -ne '0.0.0.0') { @($Bind) } else {
    [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() |
        Where-Object { $_.OperationalStatus -eq 'Up' -and $_.NetworkInterfaceType -ne 'Loopback' } |
        ForEach-Object { $_.GetIPProperties().UnicastAddresses } |
        Where-Object { $_.Address.AddressFamily -eq 'InterNetwork' } | ForEach-Object { $_.Address.ToString() }
}
Write-Host ''
Write-Host 'Connect a client with:'
foreach ($address in $addresses) { Write-Host "  Server URL:  wss://${address}:$Port" }
Write-Host "  Password:    contents of $passwordFile"
Write-Host "  Fingerprint: $fingerprint"
Write-Host "  Browser test page: https://<address>:$Port/ (accept the certificate once)"
Write-Host ''
Write-Host "Starting $Executable $($arguments -join ' ')"
& $Executable @arguments
exit $LASTEXITCODE
