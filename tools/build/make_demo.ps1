$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$renderer = Join-Path $root 'build/host/Release/echolabnt_render.exe'
if (!(Test-Path -LiteralPath $renderer)) { throw 'Construire le renderer : cmake --preset windows ; cmake --build --preset windows.' }
$python = Join-Path $root '.venv/Scripts/python.exe'
if (!(Test-Path -LiteralPath $python)) { $python = (Get-Command python -ErrorAction Stop).Source }
$analysis = Join-Path $root 'tools/analysis/echo_tools.py'
$out = Join-Path $root 'build/demo'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$excitation = Join-Path $out 'synthetic_impulse.wav'
& $python $analysis generate_stimuli --kind impulse --duration 2 --lead-seconds 0.25 --level-dbfs -18 --output $excitation
if ($LASTEXITCODE -ne 0) { throw 'Génération excitation échouée.' }
foreach ($model in @('digital', 'analog', 'tube', 'pingpong', 'multihead', '8bit', 'dynamic', 'tape', 'sweep', 'analogmod', 'digitalmod', 'reverse', 'autovolume', 'digitalrack', 'platter', 'stereo', 'looper', 'exciter', 'sweepup', 'tubea04')) {
    $delay = if ($model -eq 'looper') { 1000 } else { 250 }
    & $renderer --input $excitation --output (Join-Path $out "synthetic_$model.wav") --model $model --delay-ms $delay --feedback 0.28 --mix 1 --drive 0 --tone-hz 4500 --mod-rate-hz 0.7 --mod-depth-ms 2 --feedback-lowpass-hz 0 --feedback-highpass-hz 0 --drift-amount 0 --lores-rate-envelope-amount 0 --digital-rack-rate-hz 0 --limiter 0 --sample-rate-hz 0 --tap-pattern 0 --tail 1 --diagnostic
    if ($LASTEXITCODE -ne 0) { throw "Rendu $model échoué." }
}
& $renderer --input $excitation --output (Join-Path $out 'synthetic_time_transition.wav') --profile (Join-Path $root 'models/digital_demo.profile') --events (Join-Path $root 'models/time_transition.csv') --drive 0 --tone-hz 4500 --mod-rate-hz 0.7 --mod-depth-ms 2 --feedback-lowpass-hz 0 --feedback-highpass-hz 0 --drift-amount 0 --lores-rate-envelope-amount 0 --digital-rack-rate-hz 0 --limiter 0 --sample-rate-hz 0 --tap-pattern 0 --tail 1 --block 20 --diagnostic
if ($LASTEXITCODE -ne 0) { throw 'Rendu des transitions échoué.' }
& $python $analysis estimate_loop (Join-Path $out 'synthetic_digital.wav') --echo-starts '0.5,0.75,1.0' --window 0.05 --regularization 1e-12 --topology single_path --synthetic-fixture --output (Join-Path $out 'synthetic_loop.json')
if ($LASTEXITCODE -ne 0) { throw 'Estimation boucle synthétique échouée.' }
$files = @(Get-ChildItem -LiteralPath $out -File | Where-Object Name -ne 'demo_manifest.json' | Sort-Object Name | ForEach-Object { [ordered]@{ path=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } })
[ordered]@{ schema_version=1; status='synthetic_fixture'; purpose='Synthetic local pipeline demonstration'; files=$files } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'demo_manifest.json') -Encoding utf8
Write-Output "Démonstration synthétique : $out"
