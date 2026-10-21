<#
.SYNOPSIS
    Installs the current user's BeamIt TLS certificate for double-click startup.
.DESCRIPTION
    Run as the same Windows user that will run BeamIt; administrator rights are not required.
    Uses the legacy RSA/AES provider required by Schannel/boost-wintls, with a non-exportable private key.
    Does not add a trusted root or change firewall rules. Clients should pin the printed fingerprint.
    Existing certificates are preserved, so running the script cannot break a running host.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$certificate = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object {
        $_.FriendlyName -eq 'BeamIt self-signed TLS' -and $_.HasPrivateKey -and
        $_.NotBefore -le (Get-Date) -and $_.NotAfter -gt (Get-Date).AddDays(7) -and
        $_.PrivateKey -is [System.Security.Cryptography.RSACryptoServiceProvider]
    } |
    Sort-Object NotAfter -Descending | Select-Object -First 1

if (-not $certificate) {
    $certificate = New-SelfSignedCertificate -Subject 'CN=BeamIt host' `
        -DnsName @('localhost', [System.Net.Dns]::GetHostName()) `
        -FriendlyName 'BeamIt self-signed TLS' -CertStoreLocation 'Cert:\CurrentUser\My' `
        -Provider 'Microsoft Enhanced RSA and AES Cryptographic Provider' `
        -KeyAlgorithm RSA -KeyLength 2048 -KeySpec KeyExchange -KeyExportPolicy NonExportable `
        -HashAlgorithm SHA256 -Type SSLServerAuthentication `
        -NotBefore (Get-Date).AddMinutes(-5) -NotAfter (Get-Date).AddYears(2)
    Write-Host 'Installed the BeamIt TLS certificate in CurrentUser\My.'
} else {
    Write-Host 'Using the existing BeamIt TLS certificate in CurrentUser\My.'
}
$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $fingerprint = ($sha256.ComputeHash($certificate.RawData) | ForEach-Object { $_.ToString('X2') }) -join ':'
    Write-Host "Certificate SHA-256 fingerprint: $fingerprint"
    Write-Host "Valid until $($certificate.NotAfter). Pin this fingerprint in your BeamIt client."
} finally {
    $sha256.Dispose()
    $certificate.Dispose()
}
