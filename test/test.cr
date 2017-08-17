require "http/server"

server = HTTP::Server.new(3000) do |ctx|
  puts "hi"
  ctx.response.content_type = "text/plain"
  ctx.response.print "Hello Coat!"
end

server.listen
