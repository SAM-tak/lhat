return function(rounds: number, size: number, seed: number): number
    local total = 0
    for _ = 1, rounds do
        local values: {number} = {}
        local i = 0
        while i < size do values[i + 1] = i + seed; i += 1 end
        for _ = 1, 4 do
            i = 0
            while i < size do
                local value = (values[i + 1] or 0) + 1
                values[i + 1] = value
                total += value
                i += 1
            end
        end
    end
    return total
end
