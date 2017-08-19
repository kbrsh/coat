require "http/server"

server = HTTP::Server.new(3000) do |ctx|
  ctx.response.content_type = "text/plain"
  if ctx.request.path == "/test"
    ctx.response.print "Hello Coat! Test Route."
  else
    ctx.response.print "Hello Coat!"
  end
end

server.listen
