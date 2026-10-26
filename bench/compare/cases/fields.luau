return function(rounds: number, size: number, seed: number): number
    local total = 0
    for _ = 1, rounds do
        local object = {x = seed, y = seed + 1}
        local i = 0
        while i < size do
            object.x += 1
            object.y += 2
            total += object.x + object.y
            i += 1
        end
    end
    return total
end
