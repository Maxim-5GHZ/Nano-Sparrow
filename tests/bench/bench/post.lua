-- POST-сценарий для wrk: эхо тела (прокси со стримингом аплоада)
wrk.method = "POST"
wrk.path = "/api/echo"
wrk.headers["Content-Type"] = "application/octet-stream"
wrk.body = "payload-0f5e2a91c7b84d3e" .. string.rep("x", 1024)
