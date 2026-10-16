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
.EXAMPLE
    .\scripts\Start-BeamItServer.ps1 -Instance teams -Port 17390 -Bind 192.168.1.10
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9_.-]+$')]
    [string] $Instance = 'default',
    [ValidateRange(1, 65535)]
    [int] $Port = 17389,
    [string] $Bind = '0.0.0.0',
    # Path to BeamIt.exe; defaults to the newest cmake-build-*\BeamIt.exe in the repository.
    [string] $Executable,
    # Instance directory; defaults to %LOCALAPPDATA%\BeamIt\<Instance>.
    [string] $DataDirectory,
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

$repository = Split-Path -Parent $PSScriptRoot
if (-not $Executable) {
    $candidate = Get-ChildItem -Path $repository -Filter 'cmake-build-*' -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'BeamIt.exe' } | Where-Object { Test-Path -LiteralPath $_ } |
        Get-Item | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $candidate) { throw "BeamIt.exe not found under $repository\cmake-build-*; build it or pass -Executable." }
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
               '--password-file', $passwordFile, '--tls-cert', $certFile, '--tls-key', $keyFile) + $BeamItArguments
Write-Host "Starting $Executable $($arguments -join ' ')"
Write-Host "Open https://$([System.Net.Dns]::GetHostName()):$Port/ and accept the certificate once."
& $Executable @arguments
exit $LASTEXITCODE
