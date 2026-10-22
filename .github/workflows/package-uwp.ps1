$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$stage = Join-Path $env:GITHUB_WORKSPACE 'build/ci-uwp/package-root'
$output = Join-Path $env:GITHUB_WORKSPACE 'build/ci-uwp/artifacts'
$manifestPath = Join-Path $stage 'AppxManifest.xml'
if (-not (Test-Path $manifestPath -PathType Leaf)) {
    throw 'Pending UWP packaging integration: CMake install component Runtime must stage AppxManifest.xml, launcher, assets and runtime DLLs; see docs/CI.md'
}

[xml]$manifest = Get-Content -Raw $manifestPath
$identity = $manifest.Package.Identity
if ($identity.ProcessorArchitecture -ne 'x64' -or -not $identity.Publisher) {
    throw 'UWP manifest must specify an x64 identity and a certificate Publisher'
}
$applications = @($manifest.Package.Applications.Application)
if ($applications.Count -ne 1) { throw 'Expected exactly one UWP launcher application' }
$executable = [string]$applications[0].Executable
if (-not $executable -or $executable.Contains('$') -or
    [IO.Path]::IsPathRooted($executable) -or ($executable -split '[\\/]') -contains '..') {
    throw 'UWP manifest must reference a staged launcher using a relative executable path'
}
if (-not (Test-Path (Join-Path $stage $executable) -PathType Leaf)) {
    throw "Pending UWP launcher integration: staged executable is missing: $executable"
}
if ($applications[0].EntryPoint -eq 'Windows.FullTrustApplication') {
    throw 'Xbox requires a UWP launcher; a desktop full-trust application cannot be packaged'
}

# Use the newest complete x64 SDK tool set, as in NXbox packaging.
$sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
$sdk = Get-ChildItem $sdkBin -Directory |
    Where-Object { $_.Name -match '^10\.\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    Where-Object {
        (Test-Path (Join-Path $_.FullName 'x64/makeappx.exe')) -and
        (Test-Path (Join-Path $_.FullName 'x64/signtool.exe'))
    } | Select-Object -First 1
if (-not $sdk) { throw 'Windows SDK x64 MakeAppx and SignTool are required' }
$makeAppx = Join-Path $sdk.FullName 'x64/makeappx.exe'
$signTool = Join-Path $sdk.FullName 'x64/signtool.exe'
New-Item $output -ItemType Directory -Force | Out-Null
$package = Join-Path $output 'BotWRecomp-uwp-x64-test.appx'
$publicCertificate = Join-Path $output 'BotWRecomp-uwp-x64-test.cer'

& $makeAppx pack /d $stage /p $package /h SHA256 /o
if ($LASTEXITCODE -ne 0) { throw 'MakeAppx validation or packaging failed' }

$certificate = $null
$trustedCertificate = $null
try {
    # Match the manifest Publisher exactly. No secret, PFX or private key is uploaded.
    $certificate = New-SelfSignedCertificate -Type Custom `
        -Subject $identity.Publisher -CertStoreLocation 'Cert:\CurrentUser\My' `
        -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 `
        -KeyUsage DigitalSignature -KeyExportPolicy NonExportable `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') `
        -NotAfter (Get-Date).AddDays(7)
    Export-Certificate -Cert $certificate -FilePath $publicCertificate | Out-Null
    & $signTool sign /fd SHA256 /s My /sha1 $certificate.Thumbprint $package
    if ($LASTEXITCODE -ne 0) { throw 'Test certificate package signing failed' }

    # Trust only inside this disposable runner to verify the signed package. CurrentUser\Root
    # shows a confirmation dialog that hangs a headless runner; LocalMachine\Root does not.
    $trustedCertificate = Import-Certificate -FilePath $publicCertificate -CertStoreLocation 'Cert:\LocalMachine\Root'
    & $signTool verify /pa /v $package
    if ($LASTEXITCODE -ne 0) { throw 'Signed Appx verification failed' }

    @{
        revision = $env:GITHUB_SHA
        runId = $env:GITHUB_RUN_ID
        platform = 'uwp'
        renderer = 'd3d12'
        architecture = 'x64'
        runtimeOnly = $true
        containsGameCode = $false
        testCertificate = $true
        certificateThumbprint = $certificate.Thumbprint
        certificateExpires = $certificate.NotAfter.ToUniversalTime().ToString('o')
        packageSha256 = (Get-FileHash $package -Algorithm SHA256).Hash
        windowsSdk = $sdk.Name
    } | ConvertTo-Json | Set-Content (Join-Path $output 'build-info.json')
}
finally {
    if ($trustedCertificate) {
        Remove-Item "Cert:\LocalMachine\Root\$($trustedCertificate.Thumbprint)" -Force
    }
    if ($certificate) {
        Remove-Item "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -DeleteKey -Force
    }
}
