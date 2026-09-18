# Decode compiler output before normalizing dependency lines for Ninja.
param([string]$OutputFile, [string]$ErrorFile)
$utf8Strict = [System.Text.UTF8Encoding]::new($false, $true)
$utf8Output = [System.Text.UTF8Encoding]::new($false)
$outputStream = [Console]::OpenStandardOutput()
foreach ($path in @($OutputFile, $ErrorFile)) {
    $raw = [System.IO.File]::ReadAllBytes($path)
    try {
        $text = $utf8Strict.GetString($raw)
    } catch [System.Text.DecoderFallbackException] {
        $text = [System.Text.Encoding]::Default.GetString($raw)
    }
    # Include diagnostics contain two labels followed by an absolute Windows path.
    $pattern = '(?m)^[^:\r\n]+:\s*[^:\r\n]+:\s+((?:[A-Za-z]:[\\/]|\\\\)[^\r\n]*)'
    $text = [System.Text.RegularExpressions.Regex]::Replace($text, $pattern, 'Note: including file: $1')
    $encoded = $utf8Output.GetBytes($text)
    $outputStream.Write($encoded, 0, $encoded.Length)
}
$outputStream.Flush()
