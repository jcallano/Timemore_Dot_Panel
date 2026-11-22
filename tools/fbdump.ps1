# Captura del framebuffer de la placa de 7" por el puerto serie.
#
# El firmware (ui_test_01 y siguientes) responde a:
#   d  -> franja de la grafica y los botones (1024 x 320, desde y = 280)
#   D  -> pantalla completa (1024 x 600)
# y envia el contenido RGB565 crudo de cada framebuffer. Este script lo guarda
# como BMP y PNG. Sirve para ver lo que hay realmente en memoria frente a lo que
# muestra el panel (asi se encontro la grafica fantasma de la v0.4).
#
# Uso:  powershell -File tools\fbdump.ps1 -Port COM16 -Cmd D -OutDir capturas
# El puerto serie del firmware va a 921600 baudios. Abrir el puerto no reinicia
# la placa (.NET no activa DTR/RTS).
param([string]$Port = "COM16", [string]$Cmd = "d", [string]$OutDir = ".")

New-Item -ItemType Directory -Force $OutDir | Out-Null
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @"
using System; using System.IO;
public static class Rgb565 {
  public static void ToBmp(byte[] src, int off, int w, int h, string path) {
    int stride = (w * 3 + 3) & ~3;
    using (var fs = new FileStream(path, FileMode.Create)) using (var bw = new BinaryWriter(fs)) {
      bw.Write((ushort)0x4D42); bw.Write(54 + stride * h); bw.Write(0); bw.Write(54);
      bw.Write(40); bw.Write(w); bw.Write(h); bw.Write((ushort)1); bw.Write((ushort)24);
      bw.Write(0); bw.Write(stride * h); bw.Write(2835); bw.Write(2835); bw.Write(0); bw.Write(0);
      var row = new byte[stride];
      for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
          int i = off + (y * w + x) * 2;
          int p = src[i] | (src[i + 1] << 8);
          row[x * 3 + 0] = (byte)((p & 0x1F) << 3);
          row[x * 3 + 1] = (byte)(((p >> 5) & 0x3F) << 2);
          row[x * 3 + 2] = (byte)(((p >> 11) & 0x1F) << 3);
        }
        bw.Write(row);
      }
    }
  }
}
"@

$sp = New-Object System.IO.Ports.SerialPort $Port, 921600
$sp.ReadBufferSize = 4MB
$sp.ReadTimeout = 3000
$sp.Open()
Start-Sleep -Milliseconds 300
$sp.DiscardInBuffer()
$sp.Write($Cmd)

$ms = New-Object System.IO.MemoryStream
$buf = New-Object byte[] 65536
$deadline = (Get-Date).AddSeconds(90)
$endMark = [System.Text.Encoding]::ASCII.GetBytes("FBEND")
while ((Get-Date) -lt $deadline) {
  try { $n = $sp.Read($buf, 0, $buf.Length); $ms.Write($buf, 0, $n) } catch { }
  if ($ms.Length -gt 10) {
    $tail = [System.Text.Encoding]::ASCII.GetString($ms.ToArray(), [Math]::Max(0, $ms.Length - 400), [Math]::Min(400, $ms.Length))
    if ($tail.Contains("FBEND")) { break }
  }
}
$sp.Close()
$data = $ms.ToArray()
$text = [System.Text.Encoding]::GetEncoding(28591).GetString($data)   # 1 byte = 1 char
$pos = 0
while (($h = $text.IndexOf("FBDUMP ", $pos)) -ge 0) {
  $nl = $text.IndexOf("`n", $h)
  $f = $text.Substring($h + 7, $nl - $h - 7).Trim().Split(" ")
  $k = [int]$f[0]; $w = [int]$f[3]; $hh = [int]$f[4]
  $start = $nl + 1
  $need = $w * $hh * 2
  if ($start + $need -gt $data.Length) { Write-Output "fb$k incompleto: $($data.Length - $start) de $need bytes"; break }
  $out = Join-Path $OutDir ("fb{0}_{1}x{2}.bmp" -f $k, $w, $hh)
  [Rgb565]::ToBmp($data, $start, $w, $hh, $out)
  $png = $out -replace '\.bmp$', '.png'
  $bmp = [System.Drawing.Bitmap]::new((Resolve-Path $out).Path)
  $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Output "guardado $png"
  $pos = $start + $need
}
