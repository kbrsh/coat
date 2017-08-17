require "http/server"

response = HTTP::Client.get "http://rawgit.com/kbrsh/moon/master/dist/moon.js"

server = HTTP::Server.new(3000) do |ctx|
  ctx.response.content_type = "text/plain"
  ctx.response.print response.body
end

server.listen
