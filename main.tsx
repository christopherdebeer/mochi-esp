const html = await Deno.readTextFile(new URL('./index.html', import.meta.url));

export default function (req: Request): Response {
  return new Response(html, {
    headers: {
      'content-type': 'text/html; charset=utf-8',
      'cache-control': 'no-cache'
    }
  });
}
