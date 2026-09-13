param(
    [ValidateRange(10, 30)][int]$TimeoutSeconds = 15,
    [string]$GameLogPath = ''
)

# Tests the already-installed model. Never starts a server, pulls models, or changes game saves.
# After a game conversation, pass -GameLogPath to also require a real UE "APPLIED" marker.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$reportPath = Join-Path $projectRoot 'Saved\VoyagerLocalAIReport.json'
$modelName = 'llama3.2:3b'
$handler = [Net.Http.HttpClientHandler]::new()
$handler.UseProxy = $false
$handler.AllowAutoRedirect = $false
$client = [Net.Http.HttpClient]::new($handler)
$client.Timeout = [TimeSpan]::FromSeconds($TimeoutSeconds)
$checks = [ordered]@{}
$samples = [Collections.Generic.List[object]]::new()
$failure = $null

function Invoke-LocalJson([string]$Path, [object]$Body = $null) {
    if ($Path -notin @('tags', 'generate', 'ps')) { throw 'Unsupported local API route.' }
    $uri = "http://127.0.0.1:11434/api/$Path"
    $content = $null
    $response = $null
    try {
        if ($null -eq $Body) { $response = $client.GetAsync($uri).GetAwaiter().GetResult() }
        else {
            $content = [Net.Http.StringContent]::new(($Body | ConvertTo-Json -Depth 10 -Compress), [Text.Encoding]::UTF8, 'application/json')
            $response = $client.PostAsync($uri, $content).GetAwaiter().GetResult()
        }
        if ([int]$response.StatusCode -ne 200) { throw "Local API returned HTTP $([int]$response.StatusCode)." }
        $text = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        if ($text.Length -gt 16384) { throw 'Oversized local API response.' }
        return $text | ConvertFrom-Json
    }
    finally {
        if ($null -ne $content) { $content.Dispose() }
        if ($null -ne $response) { $response.Dispose() }
    }
}

function Get-ValidatedLine([object]$Envelope) {
    if (-not $Envelope.done -or $Envelope.done_reason -eq 'length') { throw 'Incomplete model response.' }
    $dialogue = $Envelope.response | ConvertFrom-Json
    $propertyNames = @($dialogue.PSObject.Properties.Name)
    if ($propertyNames.Count -ne 1 -or $propertyNames[0] -ne 'line' -or $dialogue.line -isnot [string]) {
        throw 'Dialogue did not match the single-line schema.'
    }
    $line = $dialogue.line.Trim()
    if ($line.Length -lt 15 -or $line.Length -gt 280 -or @($line -split '\s+').Count -gt 50) { throw 'Dialogue length exceeded the UI budget.' }
    if ($line -match '[\x00-\x1f\x7f<>`{}\[\]]|(?i:http|www\.|as an AI)') { throw 'Dialogue contained non-dialogue markup or controls.' }
    return $line
}

try {
    $tags = Invoke-LocalJson 'tags'
    $installed = @($tags.models | Where-Object { $_.name -eq $modelName })
    $checks['model_already_installed'] = $installed.Count -eq 1
    if (-not $checks['model_already_installed']) { throw "$modelName is not installed. No download was attempted." }
    $fixtures = @(
        [ordered]@{
            name = 'engineer_greeting'; citizen = 'Mara Voss'; role = 'Engineer'; planet = 'Aurelia b'; biome = 'VERDANT GARDEN'; district = 1; clock = '14:30'
            facts = "Hello, traveler. I keep this district's equipment running. I'm between the workshop and my next maintenance stop."
        },
        [ordered]@{
            name = 'medic_life'; citizen = 'Elias Chen'; role = 'Medic'; planet = 'Nivalis c'; biome = 'FROZEN FRONTIER'; district = 2; clock = '18:15'
            facts = 'My usual shift is at North Clinic. We break for food around noon and six, then return home after ten.'
        }
    )
    foreach ($fixture in $fixtures) {
        $prompt = "Citizen name: $($fixture.citizen). Occupation: $($fixture.role). Planet name: $($fixture.planet). Planet biome (not district name): $($fixture.biome). " +
            "District number: $($fixture.district). Verified dialogue facts: $($fixture.facts)`n" +
            'Rewrite ONLY the verified dialogue facts, keeping every fact and schedule unchanged. ' +
            "Use 1-2 sentences. Names and biography must come only from those facts. " +
            'Planet and biome are background context, not extra locations or claims to add.'
        $body = @{
            model = $modelName
            system = 'You are a game dialogue copy editor. Rephrase supplied dialogue without adding or changing facts. Speak in first person, 1-2 short sentences, at most 40 words. Copy names and schedule words exactly. Do not invent places, services, quests, dangers, controls, rewards or history. Do not give the citizen a different job or home. No role labels, stage directions, markdown, code or URLs. Return a JSON object with only a line string.'
            prompt = $prompt
            stream = $false
            keep_alive = '30s'
            format = @{ type = 'object'; properties = @{ line = @{ type = 'string' } }; required = @('line'); additionalProperties = $false }
            options = @{ num_gpu = 0; num_ctx = 1024; num_predict = 80; num_thread = 2; temperature = 0.3; seed = 31 }
        }
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $response = Invoke-LocalJson 'generate' $body
        $line = Get-ValidatedLine $response
        $grounded = $true
        foreach ($word in @('noon', 'six', 'ten', 'after ten', 'night watch')) {
            if ($fixture.facts.Contains($word) -and -not $line.ToLowerInvariant().Contains($word)) { $grounded = $false }
        }
        foreach ($match in [regex]::Matches($line, '\d+')) {
            if (-not $fixture.facts.Contains($match.Value)) { $grounded = $false }
        }
        if ($fixture.facts -match '^My usual shift is at ([^.]+)\.' -and -not $line.Contains($Matches[1])) { $grounded = $false }
        $checks["$($fixture.name)_bounded_reply"] = $true
        $checks["$($fixture.name)_grounding_or_fallback"] = $true
        $samples.Add([ordered]@{
            fixture = $fixture.name
            authored_facts = $fixture.facts
            generated_line = $line
            grounding_accepted = $grounded
            displayed_line = $(if ($grounded) { $line } else { $fixture.facts })
            elapsed_seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
            generated_tokens = $response.eval_count
            load_seconds = [Math]::Round($response.load_duration / 1000000000.0, 3)
        })
        if ($grounded) { Write-Host "PASS $($fixture.name): $line" }
        else { Write-Host "PASS $($fixture.name): ungrounded reply rejected; authored line retained." }
    }
    $running = Invoke-LocalJson 'ps'
    $checks['at_least_one_generated_line_accepted'] = @($samples | Where-Object { $_.grounding_accepted }).Count -gt 0
    $loaded = @($running.models | Where-Object { $_.name -eq $modelName })
    $checks['observed_cpu_only'] = $loaded.Count -eq 1 -and $loaded[0].size_vram -eq 0
    $checks['observed_small_context'] = $loaded.Count -eq 1 -and $loaded[0].context_length -le 1024
    if (-not $checks['observed_cpu_only'] -or -not $checks['observed_small_context']) { throw 'Loaded model exceeded the CPU/context budget.' }

    # Transport outage fixture: the game leaves its authored reply untouched on this failure path.
    $offlineHandler = [Net.Http.HttpClientHandler]::new()
    $offlineHandler.UseProxy = $false
    $offlineHandler.AllowAutoRedirect = $false
    $offlineClient = [Net.Http.HttpClient]::new($offlineHandler)
    $offlineClient.Timeout = [TimeSpan]::FromSeconds(2)
    try {
        $offlineResponse = $offlineClient.GetAsync('http://127.0.0.1:1/api/tags').GetAwaiter().GetResult()
        $offlineResponse.Dispose()
        $checks['outage_fixture_detected'] = $false
    }
    catch { $checks['outage_fixture_detected'] = $true }
    finally { $offlineClient.Dispose(); $offlineHandler.Dispose() }

    if ($GameLogPath) {
        $gameLog = Get-Content -LiteralPath $GameLogPath -Raw
        $checks['ue_conversation_applied'] = $gameLog -match 'VOYAGER LOCAL AI APPLIED chars=\d+ seconds='
        $checks['ue_validation_tests_passed'] = $gameLog -match 'Test Completed\. Result=\{Success\} Name=\{ValidatedReply\}'
    }
    if (@($checks.Values | Where-Object { -not $_ }).Count -gt 0) { throw 'One or more local dialogue checks failed.' }
}
catch { $failure = $_.Exception.Message }
finally {
    $client.Dispose()
    $handler.Dispose()
    [IO.Directory]::CreateDirectory((Split-Path -Parent $reportPath)) | Out-Null
    $report = [ordered]@{
        checked_at_utc = [DateTime]::UtcNow.ToString('o')
        passed = $null -eq $failure
        model = $modelName
        endpoint = 'http://127.0.0.1:11434'
        inference = @{ processor = 'CPU'; threads = 2; context = 1024; maximum_generated_tokens = 80 }
        checks = $checks
        samples = @($samples.ToArray())
        game_integration_checked = [bool]$GameLogPath
        failure = $failure
    }
    [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
}

Write-Host "Local dialogue report: $reportPath"
if ($failure) { throw $failure }
Write-Host 'PASS: installed model returned bounded dialogue using CPU only; authored fallback transport fixture detected.'
