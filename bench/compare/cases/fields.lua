return function(rounds, size, seed)
    local total = 0
    for _ = 1, rounds do
        local object = {x = seed, y = seed + 1}
        local i = 0
        while i < size do
            object.x = object.x + 1
            object.y = object.y + 2
            total = total + (object.x + object.y)
            i = i + 1
        end
    end
    return total
end
