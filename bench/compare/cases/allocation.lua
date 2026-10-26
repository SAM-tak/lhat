return function(rounds, size, seed)
    local total = 0
    for _ = 1, rounds do
        local i = 0
        while i < size do
            local object = {x = i + seed, y = i + seed + 1}
            total = total + (object.x + object.y)
            i = i + 1
        end
    end
    return total
end
